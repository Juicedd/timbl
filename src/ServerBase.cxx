/*
  Copyright (c) 1998 - 2009
  ILK  -  Tilburg University
  CNTS -  University of Antwerp
 
  This file is part of Timbl

  Timbl is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  Timbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.

  For questions and suggestions, see:
      http://ilk.uvt.nl/software.html
  or send mail to:
      Timbl@uvt.nl
*/

#include <string>
#include <cerrno>
#include <csignal>
#include "timbl/Types.h"
#include "timbl/Tree.h"
#include "timbl/Instance.h"
#include "timbl/Common.h"
#include "timbl/FdStream.h"
#include "timbl/LogStream.h"
#include "timbl/Options.h"
#include "timbl/GetOptClass.h"
#include "timbl/TimblAPI.h"
#include "timbl/SocketBasics.h"
#include "timbl/ServerBase.h"

using namespace std;

namespace Timbl {

  TimblServer::TimblServer(){
    maxConn = 25;
    serverPort = -1;
    exp = 0;
    tcp_socket = 0;
  }  

  bool TimblServer::getConfig( const string& serverConfigFile ){
    maxConn = 25;
    serverPort = -1;
    serverProtocol = "http";
    ifstream is( serverConfigFile.c_str() );
    if ( !is ){
      Error( "problem reading " + serverConfigFile );
      return false;
    }
    else {
      string line;
      while ( getline( is, line ) ){
	if ( line.empty() || line[0] == '#' )
	  continue;
	string::size_type ispos = line.find('=');
	if ( ispos == string::npos ){
	  Error( "invalid entry in: " + serverConfigFile );
	  Error( "offending line: '" + line + "'" );
	  return false;
	}
	else {
	  string base = line.substr(0,ispos);
	  string rest = line.substr( ispos+1 );
	  if ( !rest.empty() ){
	    string tmp = base;
	    lowercase(tmp);
	    if ( tmp == "maxconn" ){
	      if ( !stringTo( rest, maxConn ) ){
		Error( "invalid value for maxconn" );
		return false;
	      }
	    }
	    else if ( tmp == "port" ){
	      if ( !stringTo( rest, serverPort ) ){
		Error( "invalid value for port" );
		return false;
	      }
	    }
	    else if ( tmp == "protocol" ){
	      string protocol = rest;
	      lowercase( protocol );
	      if ( protocol != "http" && protocol != "tcp" ){
		Error( "invalid protocol" );
		return false;
	      }
	      serverProtocol = protocol;
	    }
	    else {
	      string::size_type spos = 0;
	      if ( rest[0] == '"' )
		spos = 1;
	      string::size_type epos = rest.length()-1;
	      if ( rest[epos] == '"' ) 
		--epos;
	      serverConfig[base] = rest.substr( spos, epos );
	    }
	  }
	}
      }
      if ( serverPort < 0 ){
	Error( "missing 'port=' entry in config file" );
	return false;
      }
      else
	return true;
    }
  }

  void startExperimentsFromConfig( map<std::string, std::string>& serverConfig,
				   map<string, TimblExperiment*>& experiments ){
    map<string,string>::const_iterator it = serverConfig.begin();
    while ( it != serverConfig.end() ){
      TimblOpts opts( it->second );
      string treeName;
      string trainName;
      bool mood;
      if ( opts.Find( 'f', trainName, mood ) )
	opts.Delete( 'f' );
      else if ( opts.Find( 'i', treeName, mood ) )
	opts.Delete( 'i' );
      if ( !( treeName.empty() && trainName.empty() ) ){
	TimblAPI *run = new TimblAPI( &opts, it->first );
	bool result = false;
	if ( run && run->Valid() ){
	  if ( treeName.empty() ){
	    //	    cerr << "trainName = " << trainName << endl;
	    result = run->Learn( trainName );
	  }
	  else {
	    //	    cerr << "treeName = " << treeName << endl;
	    result = run->GetInstanceBase( treeName );
	  }
	}
	if ( result ){
	  run->initExperiment();
	  experiments[it->first] = run->grabAndDisconnectExp();
	  delete run;
	  cerr << "started experiment " << it->first 
	       << " with parameters: " << it->second << endl;
	}
	else {
	  cerr << "FAILED to start experiment " << it->first 
	       << " with parameters: " << it->second << endl;
	}
      }
      else {
	cerr << "missing '-i' or '-f' option in serverconfig file" << endl;
      }
      ++it;
    }
  }

  struct childArgs{
    TimblExperiment *Mother;
    Sockets::ServerSocket *socket;
    int maxC;
    map<string, TimblExperiment*> *experiments;
  };

  void BrokenPipeChildFun( int Signal ){
    if ( Signal == SIGPIPE ){
      signal( SIGPIPE, BrokenPipeChildFun );
    }
  }

  enum CommandType { UnknownCommand, Classify, Base, 
		     Query, Set, Exit, Comment };
  
  CommandType check_command( const string& com ){
    CommandType result = UnknownCommand;
    if ( compare_nocase_n( com, "CLASSIFY" ) )
      result = Classify;
    else if ( compare_nocase_n( com, "QUERY" ) )
      result = Query;
    else if ( compare_nocase_n( com, "BASE") )
      result = Base;
    else if ( compare_nocase_n( com, "SET") )
      result = Set;
    else if ( compare_nocase_n( com, "EXIT" ) )
      result = Exit;
    else if ( com[0] == '#' )
      result = Comment;
    return result;
  }

  inline void Split( const string& line, string& com, string& rest ){
    string::const_iterator b_it = line.begin();
    while ( b_it != line.end() && isspace( *b_it ) ) ++b_it;
    string::const_iterator m_it = b_it;
    while ( m_it != line.end() && !isspace( *m_it ) ) ++m_it;
    com = string( b_it, m_it );
    while ( m_it != line.end() && isspace( *m_it) ) ++m_it;
    rest = string( m_it, line.end() );
  }  

  bool doSet( const string& Line, TimblExperiment *Exp ){
    if ( Exp->SetOptions( Line ) ){
      if ( Exp->ServerVerbosity() & CLIENTDEBUG )
	*Log(Exp->my_log()) << ": Command :" << Line << endl;
      if ( Exp->ConfirmOptions() )
	*Exp->sock_os << "OK" << endl;
    }
    else {
      if ( Exp->ServerVerbosity() & CLIENTDEBUG )
	*Log(Exp->my_log()) << ": Don't understand '" 
			    << Line << "'" << endl;
    }
    return true;
  }

  bool classifyOneLine( TimblExperiment *Exp, const string& params ){
    double Distance;
    string Distrib;
    string Answer;
    ostream *os = Exp->sock_os;
    if ( Exp->Classify( params, Answer, Distrib, Distance ) ){
      if ( Exp->ServerVerbosity() & CLIENTDEBUG )
	*Log(Exp->my_log()) << params << " --> " 
			    << Answer << " " << Distrib 
			    << " " << Distance << endl;
      *os << "CATEGORY {" << Answer << "}";
      if ( os->good() ){
	if ( Exp->Verbosity(DISTRIB) ){
	  *os << " DISTRIBUTION " <<Distrib;
	}
	if ( os->good() ){
	  if ( Exp->Verbosity(DISTANCE) ){
	    *os << " DISTANCE {" << Distance << "}";
	  }
	  if ( os->good() ){
	    if ( Exp->Verbosity(NEAR_N) ){
	      *os << " NEIGHBORS" << endl;
	      Exp->showBestNeighbors( *os );
	      *os << "ENDNEIGHBORS";
	    }
	  }
	}
      }
      if ( os->good() )
	*os << endl;
      return os->good();
    }
    else {
      if ( Exp->ServerVerbosity() & CLIENTDEBUG )
	*Log(Exp->my_log()) << ": Classify Failed on '" 
			    << params << "'" << endl;
      return false;
    }
  }
  
  
  int runFromSocket( childArgs *args ){ 
    string Line;
    Sockets::ServerSocket *sock = args->socket;
    int sockId = sock->getSockId();
    TimblExperiment *Chld = 0;
    signal( SIGPIPE, BrokenPipeChildFun );

    ostream *os = new fdostream( sockId );
    istream *is = new fdistream( sockId );
    string baseName;
    *os << "Welcome to the Timbl server." << endl;
    if ( args->experiments->empty() ){
      baseName == "default";
      *Dbg(args->Mother->my_debug()) << " Voor Create Default Client " << endl;
      Chld = args->Mother->CreateClient( sockId );
      *Dbg(args->Mother->my_debug()) << " Na Create Client " << endl;
      // report connection to the server terminal
      //
      char line[256];
      sprintf( line, "Thread %lu, on Socket %d", (uintptr_t)pthread_self(),
	       sockId );
      Chld->my_debug().message( line );
      *Log(Chld->my_log()) << line << ", started at: " 
			   << Timer::now() << endl;  
    }
    else {
      *os << "available bases: ";
      map<string,TimblExperiment*>::const_iterator it = args->experiments->begin();
      while ( it != args->experiments->end() ){
	*os << it->first << " ";
	++it;
      }
      *os << endl;
    }
    if ( getline( *is, Line ) ){
      *Dbg(args->Mother->my_log()) << "FirstLine='" << Line << "'" << endl;
      string Command, Param;
      int result = 0;
      bool go_on = true;
      *Dbg(args->Mother->my_debug()) << "running FromSocket: " << sockId << endl;
      
      do {
	string::size_type pos = Line.find('\r');
	if ( pos != string::npos )
	  Line.erase(pos,1);
	*Dbg(args->Mother->my_debug()) << "Line='" << Line << "'" << endl;
	Split( Line, Command, Param );
	switch ( check_command(Command) ){
	case Base:{
	  map<string,TimblExperiment*>::const_iterator it 
	    = args->experiments->find(Param);
	  if ( it != args->experiments->end() ){
	    baseName = Param;
	    *os << "selected base: '" << Param << "'" << endl;
	    if ( Chld )
	      delete Chld;
	    *Dbg(args->Mother->my_debug()) 
	      << " Voor Create Default Client " << endl;
	    Chld = it->second->CreateClient( sockId );
	    Chld->setExpName(string("exp-")+toString(sockId) );
	    *Dbg(args->Mother->my_debug()) << " Na Create Client " << endl;
	    // report connection to the server terminal
	    //
	    char line[256];
	    sprintf( line, "Thread %lu, on Socket %d", 
		     (uintptr_t)pthread_self(), sockId );
	    Chld->my_debug().message( line );
	    *Log(Chld->my_log()) << line << ", started at: " 
				 << Timer::now() << endl;  
	  }
	  else {
	    *os << "ERROR { Unknown basename: " << Param << "}" << endl;
	  }
	}
	  break;
	case Set:
	  if ( !Chld )
	    *os << "you haven't selected a base yet!" << endl;
	  else
	    go_on = doSet( Param, Chld );
	  break;
	case Query:
	  if ( !Chld )
	    *os << "you haven't selected a base yet!" << endl;
	  else {
	    *os << "STATUS" << endl;
	    Chld->ShowSettings( *os );
	    *os << "ENDSTATUS" << endl;
	  }
	  break;
	case Exit:
	  *os << "OK Closing" << endl;
	  delete Chld;
	  delete is;
	  delete os;
	  return result;
	  break;
	case Classify:
	  if ( !Chld )
	    *os << "you haven't selected a base yet!" << endl;
	  else {
	    if ( classifyOneLine( Chld, Param ) )
	      result++;
	    go_on = true; // HACK?
	  }
	  break;
	case Comment:
	  *os << "SKIP '" << Line << "'" << endl;
	  break;
	default:
	  if ( Chld->ServerVerbosity() & CLIENTDEBUG )
	    *Log(args->Mother->my_log()) << sockId << ": Don't understand '" 
					 << Line << "'" << endl;
	  *os << "ERROR { Illegal instruction:'" << Command << "' in line:" 
	      << Line << "}" << endl;
	  break;
	}
      }
      while ( go_on && getline( *is, Line ) );
      delete Chld;
      delete is;
      delete os;
      return result;
    }
    return 0;
  }
 

  // ***** This is the routine that is executed from a new TCP thread *******
  void *socketChild( void *arg ){
    childArgs *args = (childArgs *)arg;
    TimblExperiment *Mother = args->Mother;
    static int service_count=0;

    static pthread_mutex_t my_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&my_lock);
    // use a mutex to update the global service counter
    service_count++;
    if ( service_count > args->maxC ){
      *Mother->sock_os << "Maximum connections exceeded." << endl;
      *Mother->sock_os << "try again later..." << endl;
      pthread_mutex_unlock( &my_lock );
      cerr << "Thread " << (uintptr_t)pthread_self() << " refused " << endl;
    }
    else {
      pthread_mutex_unlock( &my_lock );
      Timer timeDone;
      timeDone.start();
      int nw = runFromSocket( args );
      *Log(Mother->my_log()) << "Thread " << (uintptr_t)pthread_self() 
			     << ", terminated at: " << Timer::now()
			     << "Total time used in this thread: " 
			     << timeDone << ", " << nw 
			     << " instances processed ";
      if ( timeDone.secs() > 0 && nw > 0 )
	*Log(Mother->my_log()) << " (" << nw/timeDone.secs() 
			       << " instances/sec)";
      *Log(Mother->my_log()) << endl;
      //
      pthread_mutex_lock(&my_lock);
      // use a mutex to update and display the global service counter
      *Log(Mother->my_log()) << "Socket Total = " << --service_count << endl;
      pthread_mutex_unlock(&my_lock);
      // close the socket and exit this thread
      delete args->socket;
      delete args;
    }
    return NULL;
  }

  void TimblServer::RunClassicServer(){
    if ( !pidFile.empty() ){
      // check validity of pidfile
      if ( pidFile[0] != '/' ) // make sure the path is absolute
	pidFile = '/' + pidFile;
      unlink( pidFile.c_str() ) ;
      ofstream pid_file( pidFile.c_str() ) ;
      if ( !pid_file ){
	*Log(exp->my_err())<< "unable to create pidfile:"<< pidFile << endl;
	*Log(exp->my_err())<< "TimblServer NOT Started" << endl;
	exit(1);
      }
    }
    if ( !logFile.empty() ){
      if ( logFile[0] != '/' ) // make sure the path is absolute
	logFile = '/' + logFile;
      ostream *tmp = new ofstream( logFile.c_str() );
      if ( tmp && tmp->good() ){
	*Log(exp->my_err()) << "switching logging to file " 
			       << logFile << endl;
	exp->my_log().associate( *tmp );
	exp->my_err().associate( *tmp );
	*Log(exp->my_log())  << "Started logging " << endl;	
	*Log(exp->my_log())  << "Server verbosity " << toString<VerbosityFlags>(exp->ServerVerbosity()) << endl;	
      }
      else {
	*Log(exp->my_err()) << "unable to create logfile: " << logFile << endl;
	*Log(exp->my_err()) << "not started" << endl;
	exit(1);
      }
    }
    else
      cerr << "NO logFile" << endl;


    map<string, TimblExperiment*> experiments;
    startExperimentsFromConfig( serverConfig, experiments );
    int start = daemon( 0, logFile.empty() );

    if ( start < 0 ){
      cerr << "failed to daemonize error= " << strerror(errno) << endl;
      exit(1);
    };
    if ( !pidFile.empty() ){
      // we have a liftoff!
      // signal it to the world
      ofstream pid_file( pidFile.c_str() ) ;
      if ( !pid_file ){
	*Log(exp->my_err())<< "unable to create pidfile:"<< pidFile << endl;
	*Log(exp->my_err())<< "TimblServer NOT Started" << endl;
	exit(1);
      }
      else {
	pid_t pid = getpid();
	pid_file << pid << endl;
      }
    }
    // set the attributes 
    pthread_attr_t attr;
    if ( pthread_attr_init(&attr) ||
	 pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED ) ){
      *Log(exp->my_err()) << "Threads: couldn't set attributes" << endl;
      exit(0);
    }
    *Log(exp->my_log()) << "Starting Server on port:" << serverPort << endl;

    pthread_t chld_thr;

    Sockets::ServerSocket server;
    string portString = toString<int>(serverPort);
    if ( !server.connect( portString ) ){
      *Log(exp->my_err()) << "failed to start Server: " 
			  << server.getMessage() << endl;
      exit(0);
    }

    if ( !server.listen( 5 ) ) {
      // maximum of 5 pending requests
      *Log(exp->my_err()) << server.getMessage() << endl;
      exit(0);
    }
    
    int failcount = 0;
    while( true ){ // waiting for connections loop
      signal( SIGPIPE, SIG_IGN );
      Sockets::ServerSocket *newSocket = new Sockets::ServerSocket();
      if ( !server.accept( *newSocket ) ){
	*Log(exp->my_err()) << server.getMessage() << endl;
	if ( ++failcount > 20 ){
	  *Log(exp->my_err()) << "accept failcount >20 " << endl;
	  *Log(exp->my_err()) << "server stopped." << endl;
	  exit(EXIT_FAILURE);
	}
	else {
	  continue;  
	}
      }
      else {
	failcount = 0;
	*Log(exp->my_log()) << "Accepting Connection #" 
			    << newSocket->getSockId()
			    << " from remote host: " 
			    << newSocket->getClientName() << endl;
	// create a new thread to process the incoming request 
	// (The thread will terminate itself when done processing
	// and release its socket handle)
	//
	childArgs *args = new childArgs();
	args->Mother = exp;
	args->socket = newSocket;
	args->maxC   = maxConn;
	args->experiments = &experiments;
	*Dbg(exp->my_debug()) << "voor pthread_create " << endl;
	pthread_create( &chld_thr, &attr, socketChild, (void *)args );
      }
      // the server is now free to accept another socket request 
    }
    pthread_attr_destroy(&attr); 
  }

#define IS_DIGIT(x) (((x) >= '0') && ((x) <= '9'))
#define IS_HEX(x) ((IS_DIGIT(x)) || (((x) >= 'a') && ((x) <= 'f')) || \
            (((x) >= 'A') && ((x) <= 'F')))


  string urlDecode( const string& s ) {
    int cc;
    string result;
    int len=s.size();
    for (int i=0; i<len ; ++i ) {
      cc=s[i];
      if (cc == '+') {
	result += ' ';
      } 
      else if ( cc == '%' &&
		( i < len-2 &&
		  ( IS_HEX(s[i+1]) ) &&
		  ( IS_HEX(s[i+2]) ) ) ){
	std::istringstream ss( "0x"+s.substr(i+1,2) );
	int tmp;
	ss >> std::showbase >> std::hex;
	ss >> tmp;
      result = result + (char)tmp;
      i += 2;
      }
      else {
	result += cc;
      }
    }
    return result;
  }

  // ***** This is the routine that is executed from a new HTTP thread *******
  void *httpChild( void *arg ){
    childArgs *args = (childArgs *)arg;
    TimblExperiment *Mother = args->Mother;
    args->socket->setNonBlocking();
    int sockId = args->socket->getSockId(); 
    fdistream is(sockId);
    fdostream os(sockId);
    map<string, TimblExperiment*> *experiments = args->experiments;
    static int service_count=0;

    static pthread_mutex_t my_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&my_lock);
    // use a mutex to update the global service counter
    service_count++;
    if ( service_count > args->maxC ){
      os << "Maximum connections exceeded." << endl;
      os << "try again later..." << endl;
      pthread_mutex_unlock( &my_lock );
      *Log(Mother->my_log()) << "Thread " << (uintptr_t)pthread_self()
			     << " refused " << endl;
    }
    else {
      pthread_mutex_unlock( &my_lock );
      // process the test material
      // report connection to the server terminal
      //
      char logLine[256];
      sprintf( logLine, "Thread %lu, on Socket %d", (uintptr_t)pthread_self(),
	       sockId );
      *Log(Mother->my_log()) << logLine << ", started at: " 
			     << Timer::now() << endl;  
      signal( SIGPIPE, BrokenPipeChildFun );
      string Line;
      int timeout = 1;
      if ( nb_getline( is, Line, timeout ) ){
	*Dbg(Mother->my_debug()) << "FirstLine='" << Line << "'" << endl;
	if ( Line.find( "HTTP" ) != string::npos ){
	  // skip HTTP header
	  string tmp;
	  timeout = 1;
	  while ( ( nb_getline( is, tmp, timeout ), !tmp.empty()) ){
	    //	    cerr << "skip: read:'" << tmp << "'" << endl;;
	  }
	  string::size_type spos = Line.find( "GET" );
	  if ( spos != string::npos ){
	    string::size_type epos = Line.find( " HTTP" );
	    string line = Line.substr( spos+3, epos - spos - 3 );
	    *Dbg(Mother->my_debug()) << "Line='" << line << "'" << endl;
	    epos = line.find( "?" );
	    string basename;
	    if ( epos != string::npos ){
	      basename = line.substr( 0, epos );
	      string qstring = line.substr( epos+1 );
	      epos = basename.find( "/" );
	      if ( epos != string::npos ){
		basename = basename.substr( epos+1 );
		map<string,TimblExperiment*>::const_iterator it= experiments->find(basename);
		if ( it != experiments->end() ){
		  TimblExperiment *api = it->second->CreateClient( args->socket->getSockId() );
		  if ( api ){
		    string name = string("exp-")+toString(sockId);
		    api->setExpName( name );
		    LogStream LS( &Mother->my_log() );
		    LogStream DS( &Mother->my_debug() );
		    DS.message(logLine);
		    LS.message(logLine);
		    DS.setstamp( StampBoth );
		    LS.setstamp( StampBoth );
		    XmlDoc doc( "TiMblResult" );
		    xmlNode *root = doc.getRoot();
		    XmlSetAttribute( root, "algorithm", toString(api->Algorithm()) );
		    vector<string> avs;
		    int avNum = split_at( qstring, avs, "&" );
		    if ( avNum > 0 ){
		      multimap<string,string> acts;
		      for ( int i=0; i < avNum; ++i ){
			vector<string> parts;
			int num = split_at( avs[i], parts, "=" );
			if ( num == 2 ){
			  acts.insert( make_pair(parts[0], parts[1]) );
			}
			else if ( num > 2 ){
			  string tmp = parts[1];
			  for( int i=2; i < num; ++i )
			    tmp += string("=")+parts[i];
			  acts.insert( make_pair(parts[0], tmp ) );
			}
			else {
			  LS << "unknown word in query "
			     << avs[i] << endl;
			}
		      }
		      typedef multimap<string,string>::const_iterator mmit;
		      pair<mmit,mmit> range = acts.equal_range( "set" );
		      mmit it = range.first;
		      while ( it != range.second ){
			string opt = it->second;
			if ( !opt.empty() && opt[0] != '-' && opt[0] != '+' )
			  opt = string("-") + opt;
			if ( Mother->ServerVerbosity() & CLIENTDEBUG )
			  DS << "set :" << opt << endl;
			if ( api->SetOptions( opt ) ){
			  if ( !api->ConfirmOptions() ){
			    os << "set " << opt << " failed" << endl;
			  }
			}
			else {
			  LS << ": Don't understand set='" 
			     << opt << "'" << endl;
			  os << ": Don't understand set='" 
			     << it->second << "'" << endl;
			}
			++it;
		      }
		      range = acts.equal_range( "show" );
		      it = range.first;
		      while ( it != range.second ){
			if ( it->second == "settings" ){
			  xmlNode *tmp = api->settingsToXML();
			  XmlAddChild( root, tmp );
			}
			else if ( it->second == "weights" ){
			  xmlNode *tmp = api->weightsToXML();
			  XmlAddChild( root, tmp );
			}
			else 
			  LS << "don't know how to SHOW: " 
			     << it->second << endl;
			
			++it;
		      }
		      range = acts.equal_range( "classify" );
		      it = range.first;
		      while ( it != range.second ){
			string params = it->second;
			params = urlDecode(params);
			int len = params.length();
			if ( len > 2 ){
			  DS << "params=" << params << endl
			     << "params[0]='" 
			     << params[0] << "'" << endl
			     << "params[len-1]='" 
			     << params[len-1] << "'" 
			     << endl;
			  
			  if ( ( params[0] == '"' && params[len-1] == '"' )
			       || ( params[0] == '\'' && params[len-1] == '\'' ) )
			    params = params.substr( 1, len-2 );
			}
			DS << "base='" << basename << "'"
			   << endl
			   << "command='classify'" 
			   << endl;
			string distrib, answer; 
			double distance;
			if ( Mother->ServerVerbosity() & CLIENTDEBUG )
			  LS << "Classify(" << params << ")" << endl;
			if ( api->Classify( params, answer, distrib, distance ) ){
			  
			  if ( Mother->ServerVerbosity() & CLIENTDEBUG )
			    LS << "resultaat: " << answer 
			       << ", distrib: " << distrib
			       << ", distance " << distance
			       << endl;
			  
			  xmlNode *cl = XmlNewChild( root, "classification" );
			  XmlNewChild( cl, "input", params );
			  XmlNewChild( cl, "category", answer );
			  if ( api->Verbosity(DISTRIB) ){
			    XmlNewChild( cl, "distribution", distrib );
			  }
			  if ( api->Verbosity(DISTANCE) ){
			    XmlNewChild( cl, "distance",
					 toString<double>(distance) );
			  }
			  if ( api->Verbosity(NEAR_N) ){
			    xmlNode *nb = api->bestNeighborsToXML();
			    XmlAddChild( cl, nb );
			  }
			}
			else {
			  DS << "classification failed" << endl;
			}
			++it;
		      }
		    }
		    string tmp = doc.toString();
		    // cerr << "THE DOCUMENT for sending!" << endl << tmp << endl;
		    int timeout=10;
		    nb_putline( os, tmp , timeout );
		    delete api;
		  }
		}
		else {
		  *Dbg(Mother->my_log()) << "invalid BASE! '" << basename 
					 << "'" << endl;
		  os << "invalid basename: '" << basename << "'" << endl;
		}
		os << endl;
	      }
	    } 
	  }
	}
      }
      
      *Log(Mother->my_log()) << "Thread " << (uintptr_t)pthread_self() 
			     << ", terminated at: " << Timer::now() << endl;
      os.flush();
      //
      // close the socket and exit this thread
      delete args->socket;
      delete args;
      pthread_mutex_lock(&my_lock);
      // use a mutex to update and display the global service counter
      *Log(Mother->my_log()) << "Socket Total = " << --service_count << endl;
      pthread_mutex_unlock(&my_lock);
      //
    }
    return NULL;
  }
  
  void TimblServer::RunHttpServer(){
    if ( !pidFile.empty() ){
      // check validity of pidfile
      if ( pidFile[0] != '/' ) // make sure the path is absolute
	pidFile = '/' + pidFile;
      unlink( pidFile.c_str() ) ;
      ofstream pid_file( pidFile.c_str() ) ;
      if ( !pid_file ){
	*Log(exp->my_err())<< "unable to create pidfile:"<< pidFile << endl;
	*Log(exp->my_err())<< "TimblServer NOT Started" << endl;
	exit(1);
      }
    }
    if ( !logFile.empty() ){
      if ( logFile[0] != '/' ) // make sure the path is absolute
	logFile = '/' + logFile;
      ostream *tmp = new ofstream( logFile.c_str() );
      if ( tmp && tmp->good() ){
	*Log(exp->my_err()) << "switching logging to file " 
			       << logFile << endl;
	exp->my_log().associate( *tmp );
	exp->my_err().associate( *tmp );
	*Log(exp->my_log())  << "Started logging " << endl;	
	*Log(exp->my_log())  << "Server verbosity " << toString<VerbosityFlags>( exp->ServerVerbosity()) << endl;	
      }
      else {
	*Log(exp->my_err()) << "unable to create logfile: " << logFile << endl;
	*Log(exp->my_err()) << "not started" << endl;
	exit(1);
      }
    }

    map<string, TimblExperiment*> experiments;
    startExperimentsFromConfig( serverConfig, experiments );
    
    int start = daemon( 0, logFile.empty() );

    if ( start < 0 ){
      cerr << "failed to daemonize error= " << strerror(errno) << endl;
      exit(1);
    };
    if ( !pidFile.empty() ){
      // we have a liftoff!
      // signal it to the world
      ofstream pid_file( pidFile.c_str() ) ;
      if ( !pid_file ){
	*Log(exp->my_err())<< "unable to create pidfile:"<< pidFile << endl;
	*Log(exp->my_err())<< "TimblServer NOT Started" << endl;
	exit(1);
      }
      else {
	pid_t pid = getpid();
	pid_file << pid << endl;
      }
    }
    // set the attributes 
    pthread_attr_t attr;
    if ( pthread_attr_init(&attr) ||
	 pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED ) ){
      *Log(exp->my_err()) << "Threads: couldn't set attributes" << endl;
      exit(0);
    }
    *Log(exp->my_log()) << "Starting Server on port:" << serverPort << endl;

    pthread_t chld_thr;

    Sockets::ServerSocket server;
    string portString = toString<int>(serverPort);
    if ( !server.connect( portString ) ){
      *Log(exp->my_err()) << "failed to start Server: " 
			     << server.getMessage() << endl;
      exit(0);
    }

    if ( !server.listen( 5 ) ) {
      // maximum of 5 pending requests
      *Log(exp->my_err()) << server.getMessage() << endl;
      exit(0);
    }
    
    int failcount = 0;
    while( true ){ // waiting for connections loop
      signal( SIGPIPE, SIG_IGN );
      Sockets::ServerSocket *newSocket = new Sockets::ServerSocket();
      if ( !server.accept( *newSocket ) ){
	*Log(exp->my_err()) << server.getMessage() << endl;
	if ( ++failcount > 20 ){
	  *Log(exp->my_err()) << "accept failcount >20 " << endl;
	  *Log(exp->my_err()) << "server stopped." << endl;
	  exit(EXIT_FAILURE);
	}
	else {
	  continue;  
	}
      }
      else {
	failcount = 0;
	*Log(exp->my_log()) << "Accepting Connection #" 
			       << newSocket->getSockId()
			       << " from remote host: " 
			       << newSocket->getClientName() << endl;
	// create a new thread to process the incoming request 
	// (The thread will terminate itself when done processing
	// and release its socket handle)
	//
	childArgs *args = new childArgs();
	args->Mother = exp;
	args->socket = newSocket;
	args->maxC   = maxConn;
	args->experiments = &experiments;
	pthread_create( &chld_thr, &attr, httpChild, (void *)args );
      }
      // the server is now free to accept another socket request 
    }
    pthread_attr_destroy(&attr); 
  }
  
  
  bool TimblServer::startClassicServer( int port , int maxC ){
    serverPort = port;
    if ( maxC > 0 )
      maxConn = maxC;
    Info( "Starting a classic server on port " + toString( serverPort ) );
    if ( exp && exp->ConfirmOptions() ){
      exp->initExperiment( true );
      RunClassicServer();
    }
    else {
      Error( "invalid options" );
    }
    return false;
  }

  bool TimblServer::startMultiServer( const string& config ){
    if ( exp && exp->ConfirmOptions() ){
      if ( getConfig( config ) ){
	if ( serverProtocol == "http" ){
	  Info( "Starting a HTTP server on port " + toString( serverPort ) );
	  RunHttpServer();
	}
	else {
	  Info( "Starting a TCP server on port " + toString( serverPort ) );
	  RunClassicServer();
	}
      }
      else {
	Error( "invalid serverconfig" );
      }
    }
    else {
      Error( "invalid options" );
    }
    return false;
  }
      

  IB1_Server::IB1_Server( GetOptClass *opt ){
    exp = new IB1_Experiment( opt->MaxFeatures() );
    if (exp ){
      exp->UseOptions( opt );
      logFile = opt->getLogFile();
      pidFile = opt->getPidFile();
    }
  }

  IG_Server::IG_Server( GetOptClass *opt ){
    exp = new IG_Experiment( opt->MaxFeatures() );
    if (exp ){
      exp->UseOptions( opt );
      logFile = opt->getLogFile();
      pidFile = opt->getPidFile();
    }
  }

  TRIBL_Server::TRIBL_Server( GetOptClass *opt ){
    exp = new TRIBL_Experiment( opt->MaxFeatures() );
    if (exp ){
      exp->UseOptions( opt );
      logFile = opt->getLogFile();
      pidFile = opt->getPidFile();
    }
  }

  TRIBL2_Server::TRIBL2_Server( GetOptClass *opt ){
    exp = new TRIBL2_Experiment( opt->MaxFeatures() );
    if (exp ){
      exp->UseOptions( opt );
      logFile = opt->getLogFile();
      pidFile = opt->getPidFile();
    }
  }

}