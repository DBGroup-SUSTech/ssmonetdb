#include "config.h"

#include <iostream>
#include <chrono>
#include <fstream>

#ifndef HAVE_BOOST_REGEX
#include <regex>
#else
#include <boost/regex.hpp>
using boost::regex;
using boost::smatch;
using boost::regex_match;
#endif

#include <thread>
#include <typeinfo>

#include "random.hh"
#include "grammar.hh"
#include "relmodel.hh"
#include "schema.hh"
#include "gitrev.h"

#include "log.hh"
#include "dump.hh"
#include "impedance.hh"
#include "dut.hh"

#ifdef HAVE_LIBSQLITE3
#include "sqlite.hh"
#endif

#include "postgres.hh"

#include "monetdb.hh"

using namespace std;

using namespace std::chrono;

extern "C" {
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
}

/* make the cerr logger globally accessible so we can emit one last
   report on SIGINT */
cerr_logger *global_cerr_logger;

extern "C" void cerr_log_handler(int)
{
  if (global_cerr_logger)
    global_cerr_logger->report();
  exit(1);
}

int main(int argc, char *argv[])
{
  cerr << PACKAGE_NAME " " GITREV << endl;

  map<string,string> options;
  regex optregex("--(help|log-to|verbose|target|sqlite|monetdb|version|dump-all-graphs|seed|dry-run|max-queries)(?:=((?:.|\n)*))?");
  
  for(char **opt = argv+1 ;opt < argv+argc; opt++) {
    smatch match;
    string s(*opt);
    if (regex_match(s, match, optregex)) {
      options[string(match[1])] = match[2];
    } else {
      cerr << "Cannot parse option: " << *opt << endl;
      options["help"] = "";
    }
  }

  if (options.count("help")) {
    cerr <<
      "    --target=connstr     postgres database to send queries to" << endl <<
#ifdef HAVE_LIBSQLITE3
      "    --sqlite=URI         SQLite database to send queries to" << endl <<
#endif
      "    --monetdb=connstr    monetdb database to send queries to" << endl <<
      "    --log-to=connstr     log errors to postgres database" << endl <<
      "    --seed=int           seed RNG with specified int instead of PID" << endl <<
      "    --dump-all-graphs    dump generated ASTs" << endl <<
      "    --dry-run            print queries instead of executing them" << endl <<
      "    --max-queries=long   terminate after generating this many queries" << endl <<
      "    --verbose            emit progress output" << endl <<
      "    --version            print version information and exit" << endl <<
      "    --help               print available command line options and exit" << endl;
    return 0;
  } else if (options.count("version")) {
    return 0;
  }

  try
    {
      shared_ptr<schema> schema;
      if (options.count("sqlite")) {
#ifdef HAVE_LIBSQLITE3
	cout << "=====: " << options["sqlite"] << " :======" << endl;
	schema = make_shared<schema_sqlite>(options["sqlite"]);
#else
	cerr << "Sorry, " PACKAGE_NAME " was compiled without SQLite support." << endl;
	return 1;
#endif
      }
      else if(options.count("monetdb"))
      {
	cout << "=====: " << options["monetdb"] << " :======" << endl;
	schema = make_shared<schema_monetdb>(options["monetdb"]);
      }
      else
      {
	cout << "=====: " << options["target"] << " :======" << endl;
	schema = make_shared<schema_pqxx>(options["target"]);
      }


      scope scope;
      long queries_generated = 0;
      schema->fill_scope(scope);
//       work w(c);
//       w.commit();

      vector<shared_ptr<logger> > loggers;

      loggers.push_back(make_shared<impedance_feedback>());

      if (options.count("log-to"))
	loggers.push_back(make_shared<pqxx_logger>(
	     options.count("sqlite") ? options["sqlite"] : options["target"],
	     options["log-to"], *schema));

      if (options.count("verbose")) {
	auto l = make_shared<cerr_logger>();
	global_cerr_logger = &*l;
	loggers.push_back(l);
	signal(SIGINT, cerr_log_handler);
      }
      
      if (options.count("dump-all-graphs"))
	loggers.push_back(make_shared<ast_logger>());
      
      smith::rng.seed(options.count("seed") ? stoi(options["seed"]) : getpid());

      if (options.count("dry-run")) {
	while (1) {
	  shared_ptr<prod> gen = statement_factory(&scope);
	  gen->out(cout);
	  for (auto l : loggers)
	    l->generated(*gen);
	  cout << ";" << endl;
	  queries_generated++;

	  if (options.count("max-queries")
	      && (queries_generated >= stol(options["max-queries"])))
	      return 0;
	}
      }

      shared_ptr<dut_base> dut;

      if (options.count("sqlite")) {
#ifdef HAVE_LIBSQLITE3
	dut = make_shared<dut_sqlite>(options["sqlite"]);
#else
	cerr << "Sorry, " PACKAGE_NAME " was compiled without SQLite support." << endl;
	return 1;
#endif
      }
      else if(options.count("monetdb"))
      {
	dut = make_shared<dut_monetdb>(options["monetdb"]);
      }
      else
	dut = make_shared<dut_pqxx>(options["target"]);

      std::ofstream qrylog("ssquery.log");
	  std::ofstream allqueries("allqueries.log");

      while (1)
      {
	try {
	  while (1) {
	    if (options.count("max-queries")
		&& (++queries_generated > stol(options["max-queries"]))) {
	      if (global_cerr_logger)
		global_cerr_logger->report();
	      return 0;
	    }
	
	    //cout << "let's go to statement factory" <<endl;    
	    /* Invoke top-level production to generate AST */
	    shared_ptr<prod> gen = statement_factory(&scope);
	
	    for (auto l : loggers)
	      l->generated(*gen);
	  
	    /* Generate SQL from AST */
	    ostringstream s;
	    gen->out(s);
	
	    //std::cout << "Query: " << s.str() << std::endl << std::endl << std::endl;
	    allqueries << s.str() << ";" << std::endl;


	    /* Try to execute it */
	    try {
          std::ofstream qrycur("ssquery.current");
          qrycur << s.str() << ";" << std::endl;
          qrycur.close();
          clock_t starttime = clock();
	      dut->test(s.str());
	      for (auto l : loggers)
		  {	
			l->executed(*gen);
		  }
          qrylog<< "-- TIMING " << ((double)clock() - starttime) / (double) CLOCKS_PER_SEC << " seconds "<<std::endl;
		  qrylog << s.str() << ";" << std::endl;
	    } catch (const dut::failure &e) {
	      for (auto l : loggers)
		try {
		  l->error(*gen, e);
		} catch (runtime_error &e) {
		  cerr << endl << "log failed: " << typeid(*l).name() << ": "
		       << e.what() << endl;
		}
	      if ((dynamic_cast<const dut::broken *>(&e))) {
		/* re-throw to outer loop to recover session. */
		throw;
	      }
	    }
	  }
	}
	catch (const dut::broken &e) {
	  /* Give server some time to recover. */
	  this_thread::sleep_for(milliseconds(1000));
	}
      }
      qrylog.close();
	  allqueries.close();
    }
    catch (const exception &e) {
	cerr << e.what() << endl;
	return 1;
  }

}
