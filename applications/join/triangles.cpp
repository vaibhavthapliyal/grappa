/*
 * Example of:
 * (partial) triangle listing
 *
 * A(x,z) :- R(x,y), R(y,z), R(z,x)
 */


// util includes
#include "Tuple.hpp"
#include "relation_IO.hpp"


// Grappa includes
#include <Grappa.hpp>
#include "MatchesDHT.hpp"
#include <Cache.hpp>
#include <ParallelLoop.hpp>
#include <GlobalCompletionEvent.hpp>

// command line parameters
DEFINE_uint64( numTuples, 32, "Number of tuples to generate" );
DEFINE_string( in, "", "Input file relation" );
DEFINE_bool( print, false, "Print results" );

using namespace Grappa;

std::ostream& operator<<( std::ostream& o, const Tuple& t ) {
  o << "T( ";
  for (uint64_t i=0; i<TUPLE_LEN; i++) {
    o << t.columns[i] << ", ";
  }
  o << ")";
}

typedef uint64_t Column;

uint64_t identity_hash( int64_t k ) {
  return k;
}

// local portal to DHT
typedef MatchesDHT<int64_t, Tuple, identity_hash> DHT_type;
DHT_type joinTable;

// local RO copies
GlobalAddress<Tuple> local_tuples;
Column local_join1Left, local_join1Right, local_join2Right;
Column local_select;
GlobalAddress<Tuple> IndexBase;


// TODO: incorporate the edge tuples generation (although only does triples)
void generate_data( GlobalAddress<Tuple> base, size_t num ) {
  forall_localized(base, num, [](int64_t j, Tuple& t) {
    Tuple r;
    for ( uint64_t i=0; i<TUPLE_LEN; i++ ) {
      r.columns[i] = rand()%FLAGS_numTuples; 
    }
    t = r;
  });
}

void scanAndHash( GlobalAddress<Tuple> tuples, size_t num ) {
  forall_localized( tuples, num, [](int64_t i, Tuple& t) {
    int64_t key = t.columns[local_join1Left];

    VLOG(2) << "insert " << key << " | " << t;
    joinTable.insert( key, t );
  });
}


void triangles( GlobalAddress<Tuple> tuples, Column ji1, Column ji2, Column ji3, Column s ) {
  // initialization
  on_all_cores( [tuples, ji1, ji2, ji3, s] {
    local_tuples = tuples;
    local_join1Left = ji1;
    local_join1Right = ji2;
    local_join2Right = ji3;
    local_select = s;
    local_triangle_count = 0;
  });
    
  // scan tuples and hash join col 1
  VLOG(1) << "Scan tuples, creating index on subject";
  
  double start, end;
  start = Grappa_walltime();
  {
    scanAndHash( tuples, FLAGS_numTuples );
  } 
  end = Grappa_walltime();
  
  VLOG(1) << "insertions: " << (end-start)/FLAGS_numTuples << " per sec";

#if DEBUG
  printAll(tuples, FLAGS_numTuples);
#endif

  // tell the DHT we are done with inserts
  VLOG(1) << "DHT setting to RO";
#if SORTED_KEYS
  GlobalAddress<Tuple> index_base = DHT_type::set_RO_global( &joinTable );
  on_all_cores([index_base] {
      IndexBase = index_base;
  });
#else
  DHT_type::set_RO_global( &joinTable );
#endif

  start = Grappa_walltime();
  VLOG(1) << "Starting 1st join";
  forall_localized( tuples, FLAGS_numTuples, [](int64_t i, Tuple& t) {
    int64_t key = t.columns[local_join1Right];
   
    // will pass on this first vertex to compare in the select 
    int64_t x1 = t.columns[local_join1Left];

#if SORTED_KEYS
    // first join
    uint64_t results_idx;
    size_t num_results = joinTable.lookup( key, &results_idx );
    DVLOG(4) << "key " << t << " finds (" << results_idx << ", " << num_results << ")";
   
    // iterate over the first join results in parallel
    // (iterations must spawn with synch object `local_gce`)
    forall_here_async_public( results_idx, num_results, [x1](int64_t start, int64_t iters) {
      Tuple subset_stor[iters];
      Incoherent<Tuple>::RO subset( IndexBase+start, iters, &subset_stor );
#else // MATCHES_DHT
    // first join
    GlobalAddress<Tuple> results_addr;
    size_t num_results = joinTable.lookup( key, &results_addr );
    
    // iterate over the first join results in parallel
    // (iterations must spawn with synch object `local_gce`)
    /* not yet supported: forall_here_async_public< GCE=&local_gce >( 0, num_results, [x1,results_addr](int64_t start, int64_t iters) { */
    forall_here_async( 0, num_results, [x1,results_addr](int64_t start, int64_t iters) { 
      Tuple subset_stor[iters];
      Incoherent<Tuple>::RO subset( results_addr+start, iters, subset_stor );
#endif

      for ( int64_t i=0; i<iters; i++ ) {

        int64_t key = subset[i].columns[local_join2Right];

        int64_t x2 = subset[i].columns[0];
        
        if ( !(x1 < x2) ) {  // early select on ordering
          continue;
        }

#if SORTED_KEYS
        // second join
        uint64_t results_idx;
        size_t num_results = joinTable.lookup( key, &results_idx );

        VLOG(1) << "results key " << key << " (n=" << num_results;
        
        // iterate over the second join results in parallel
        // (iterations must spawn with synch object `local_gce`)
        forall_here_async_public( results_idx, num_results, [x1](int64_t start, int64_t iters) {
          Tuple subset_stor[iters];
          Incoherent<Tuple>::RO subset( IndexBase+start, iters, subset_stor );
#else // MATCHES_DHT
        // second join
        GlobalAddress<Tuple> results_addr;
        size_t num_results = joinTable.lookup( key, &results_addr );

        VLOG(1) << "results key " << key << " (n=" << num_results;
        
        // iterate over the second join results in parallel
        // (iterations must spawn with synch object `local_gce`)
        /* not yet supported: forall_here_async_public< GCE=&local_gce >( 0, num_results, [x1,results_addr](int64_t start, int64_t iters) { */
        forall_here_async( 0, num_results, [x1,x2,results_addr](int64_t start, int64_t iters) {
          Tuple subset_stor[iters];
          Incoherent<Tuple>::RO subset( results_addr+start, iters, subset_stor );
#endif
          
          for ( int64_t i=0; i<iters; i++ ) {
            int64_t r = subset[i].columns[0];
            if ( x2 < r ) {  // select on ordering 
              if ( subset[i].columns[local_select] == x1 ) { // select on triangle
                if (FLAGS_print) {
                  VLOG(1) << x1 << " " << x2 << " " << r;
                }
                local_triangle_count++;
              }
            }
          }
        }); // end loop over 2nd join results
      }
    }); // end loop over 1st join results
  }); // end outer loop over tuples
         
  
      end = Grappa_walltime();
  VLOG(1) << "joins: " << (end-start) << " seconds";
}

void user_main( int * ignore ) {

  GlobalAddress<Tuple> tuples = Grappa_typed_malloc<Tuple>( FLAGS_numTuples );

  if ( FLAGS_in == "" ) {
    VLOG(1) << "Generating some data";
    generate_data( tuples, FLAGS_numTuples );
  } else {
    VLOG(1) << "Reading data from " << FLAGS_in;
    readTuples( FLAGS_in, tuples, FLAGS_numTuples );
  }

  DHT_type::init_global_DHT( &joinTable, 64 );

  Column joinIndex1 = 0; // subject
  Column joinIndex2 = 1; // object
  Column select = 1; // object

  // triangle (assume one index to build)
  triangles( tuples, joinIndex1, joinIndex2, joinIndex2, select ); 
}


/// Main() entry
int main (int argc, char** argv) {
    Grappa_init( &argc, &argv ); 
    Grappa_activate();

    Grappa_run_user_main( &user_main, (int*)NULL );
    CHECK( Grappa_done() == true ) << "Grappa not done before scheduler exit";
    Grappa_finish( 0 );
}



// insert conflicts use java-style arraylist, enabling memcpy for next step of join
