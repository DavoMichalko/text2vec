// Copyright (C) 2015 - 2016  Dmitriy Selivanov
// This file is part of text2vec
//
// text2vec is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// text2vec is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with text2vec.  If not, see <http://www.gnu.org/licenses/>.

#include "Corpus.h"
// header from digest package
#include <pmurhashAPI.h>

using namespace Rcpp;
using namespace std;

// seeds for hashing trick
const uint32_t MURMURHASH3_HASH_SEED = 3120602769LL;
const uint32_t MURMURHASH3_SIGN_SEED = 79193439LL;

// feature hash
uint32_t murmurhash3_hash ( string &str) {
  return PMurHash32(MURMURHASH3_HASH_SEED, str.c_str(), str.size());
}

//[[Rcpp::export]]
IntegerVector hasher(CharacterVector x, int hash_size) {
  IntegerVector res(x.size());
  const char * ptr;
  for (auto i = 0; i < x.size(); i++) {
    ptr = x[i].begin();
    res[i] = PMurHash32(MURMURHASH3_HASH_SEED, ptr, strlen(ptr)) % hash_size;
  }
  return res;
}


// feature sign hash
int murmurhash3_sign (const string &str) {
  return (int)PMurHash32(MURMURHASH3_SIGN_SEED, str.c_str(), str.size());
}

//-----------------------------------------------------------------
// HashCorpus class definitions
//-----------------------------------------------------------------
class HashCorpus: public Corpus {
public:
  // constructor
  HashCorpus(uint32_t size,
             uint32_t ngram_min, uint32_t ngram_max,
             uint32_t win_size,
             int use_signed_hash);
  // total number of tokens in corpus
  int get_token_count() {return this -> token_count;};
  int get_doc_count() { return this -> doc_count; };

  void clear_tcm() {this->tcm.clear();};
  size_t get_tcm_size() {return this->tcm.size();};

  // implements hashing trick
  void insert_terms (vector< string> &terms, int grow_dtm, int context);

  void insert_document(const CharacterVector doc, int grow_dtm = 1, int context = 0);
  void insert_document_batch(const ListOf<const CharacterVector> docs_batch,
                             int grow_dtm = 1, int context = 0);

  // get term cooccurence matrix
  SEXP get_tcm() {
    CharacterVector dummy_dimnames(0);
    return tcm.get_sparse_triplet_matrix(dummy_dimnames, dummy_dimnames);
  }

  SEXP get_dtm_triplet() {
    CharacterVector dummy_names(0);
    return dtm.get_sparse_triplet_matrix(dummy_names, dummy_names);
  };

  // R's interface to document-term matrix construction
  SEXP get_dtm() { return get_dtm_triplet();};

private:
  uint32_t buckets_size;
  int signed_hash;
};

//-----------------------------------------------------------------
// implementation HashCorpus methods
//-----------------------------------------------------------------
HashCorpus::HashCorpus(uint32_t size,
           uint32_t ngram_min, uint32_t ngram_max,
           uint32_t win_size,
           int use_signed_hash)
{
  doc_count = 0;
  token_count = 0;
  buckets_size = size;
  signed_hash = use_signed_hash;
  this->ngram_min = ngram_min;
  this->ngram_max = ngram_max;
  this->window_size = win_size;
  this->ngram_delim = "_";
  // init dtm with ncol = hash_size
  dtm = SparseTripletMatrix<int>(0, size);
  tcm = SparseTripletMatrix<float>(size, size);
};
//-----------------------------------------------------------------
// int context = 0 means symmetric context for co-occurence - matrix will be symmetric
// So we will keep only right upper-diagonal elements
// int context = 1 means right words context only
// int context = -1 means left words context only
void HashCorpus::insert_terms (vector< string> &terms, int grow_dtm, int context) {
  uint32_t term_index, context_term_index;

  size_t K = terms.size();
  size_t i = 0;
  float increment = 0.0;

  for(auto term: terms) {
    this->token_count++;
    term_index = murmurhash3_hash(term) % buckets_size;
    if(grow_dtm) {
      int wcnt = 1;
      if(signed_hash && murmurhash3_sign(term) < 0)
        wcnt = -1;
      dtm.add(doc_count, term_index, wcnt);
    }
    //###########################################
    // cooccurence related
    // will check 1 == ngram_min == ngram_max on R side
    // and set window_size = 0 if not
    // will not go into this loop if window_size == 0
    for (uint32_t j = 1; j <= this->window_size; j++) {
      // check doc bounds
      if( i + j < K) {
        context_term_index = murmurhash3_hash(terms[i + j]) % buckets_size;
        // calculate cooccurence increment for particular position j of context word
        increment = weighting_fun(j);
        // int context = 0 means symmetric context for co-occurence - matrix will be symmetric
        // So we will keep only right upper-diagonal elements
        // int context = 1 means right words context only
        // int context = -1 means left words context only
        switch(context) {
        // handle symmetric context
        case 0:
          // map stores only elements above diagonal because our matrix is symmetrical
          if(term_index < context_term_index)
            this->tcm.add(term_index, context_term_index, increment);
          else {
            // also we are not interested in context words equal to main word
            // diagonal elememts will be zeros
            // if(term_index != context_term_index)
            // commented out because experiments showed that it is better to have diagonal elements
            this->tcm.add(context_term_index, term_index, increment);
            // since diagonal matrix will be passed 2 times for symmetric matrix
            // (we do 2 pass in glove for)
            // worth to think about dividing weights by 2
            //this->tcm.add(context_term_index, term_index, increment / 2);
          }
          break;
          // handle right context
        case 1:
          this->tcm.add(term_index, context_term_index, increment);
          break;
          // handle left context
        case -1:
          this->tcm.add(context_term_index, term_index, increment);
          break;
        default:
          stop("call to insert_terms with context !in [0,1, -1]");
        }
      }
    }
    i++;
  }
}

void HashCorpus::insert_document(const CharacterVector doc, int grow_dtm, int context) {
  checkUserInterrupt();
  generate_ngrams(doc, this->ngram_min, this->ngram_max,
                  this->stopwords,
                  this->terms_filtered_buffer,
                  this->ngrams_buffer,
                  this->ngram_delim);
  insert_terms(this->ngrams_buffer, grow_dtm, context);
  this->dtm.increment_nrows();
  this->doc_count++;
}

void HashCorpus::insert_document_batch(const ListOf<const CharacterVector> docs_batch,
                                       int grow_dtm, int context) {
  for (auto it:docs_batch)
    insert_document(it, grow_dtm, context);
}
