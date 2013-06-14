/*  This file is part of Jellyfish.

    Jellyfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Jellyfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Jellyfish.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef __BLOOM_COUNTER2_HPP__
#define __BLOOM_COUNTER2_HPP__

#include <assert.h>
#include <math.h>
#include <limits.h>
#include <jellyfish/mapped_file.hpp>
#include <jellyfish/allocators_mmap.hpp>
#include <jellyfish/divisor.hpp>
#include <jellyfish/atomic_gcc.hpp>
#include <jellyfish/atomic_field.hpp>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

namespace jellyfish {
template<typename Key>
struct hash_pair { };

/* Bloom counter with 3 values: 0, 1 or 2. It is thread safe and lock free.
 */
template<typename Key, typename HashPair = hash_pair<Key>, typename atomic_t = ::atomic::gcc>
class bloom_counter2_base {
  // The number of bits in the structure, previously known as m_, is
  // know stored as d_.d()
  const jflib::divisor64 d_;
  const unsigned long    k_;
  unsigned char * const  data_;
  HashPair               hash_fns_;
  atomic_t               atomic_;

  struct prefetch_info {
    size_t         boff;
    unsigned char* pos;
  };

protected:
  static size_t nb_bytes(size_t l) {
    return l / 5 + (l % 5 != 0);
  }

public:
  typedef Key key_type;

  bloom_counter2_base(size_t m, unsigned long k, unsigned char* ptr, const HashPair& fns = HashPair()) :
    d_(m), k_(k), data_(ptr), hash_fns_(fns) { }

  bloom_counter2_base(const bloom_counter2_base& rhs) = delete;
  bloom_counter2_base(bloom_counter2_base&& rhs) :
    d_(rhs.d_), k_(rhs.k_), data_(data_), hash_fns_(std::move(rhs.hash_fns_)) { }


  void write_bits(std::ostream& out) {
    out.write((char*)data_, nb_bytes(d_.d()));
  }

  // Number of hash functions
  unsigned long k() const { return k_; }
  // Size of bit vector
  size_t m() const { return d_.d(); }
  const HashPair& hash_functions() const { return hash_fns_; }

  // Insert key k. Returns previous value of k
  unsigned int insert(const Key &k) {
    uint64_t hashes[2];
    hash_fns_(k, hashes);
    return insert(hashes);
  }

  // Insert key with given hashes
  unsigned int insert(const uint64_t* hashes) {
    // Prefetch memory locations
    static_assert(std::is_pod<prefetch_info>::value, "prefetch_info must be a POD");
    prefetch_info pinfo[k()];
    const size_t base = d_.remainder(hashes[0]);
    const size_t inc  = d_.remainder(hashes[1]);
    for(unsigned long i = 0; i < k_; ++i) {
      const size_t p   = d_.remainder(base + i * inc);
      const size_t off = p / 5;
      pinfo[i].boff    = p % 5;
      pinfo[i].pos     = data_ + off;
      //      prefetch_write_no(pinfo[i].pos);
      __builtin_prefetch(pinfo[i].pos, 1, 0);
    }

    // Insert element
    unsigned char res = 2;
    for(unsigned long i = 0; i < k_; ++i) {
      size_t        boff = pinfo[i].boff;
      unsigned char v    = jflib::a_load(pinfo[i].pos);

      while(true) {
        unsigned char w = v;
        switch(boff) {
        case 0:          break;
        case 1: w /= 3;  break;
        case 2: w /= 9;  break;
        case 3: w /= 27; break;
        case 4: w /= 81; break;
        }
        w = w % 3;
        if(w == 2) break;
        unsigned char nv = v;

        switch(boff) {
        case 0: nv += 1;  break;
        case 1: nv += 3;  break;
        case 2: nv += 9;  break;
        case 3: nv += 27; break;
        case 4: nv += 81; break;
        }
        unsigned char cv = atomic_.cas(pinfo[i].pos, v, nv);
        if(cv == v) {
          if(w < res)
            res = w;
          break;
        }
        v = cv;
      }
    }
    return res;
  }

  unsigned int check(const Key &k) const {
    uint64_t hashes[2];
    hash_fns_(k, hashes);
    return check(hashes);
  }

  unsigned int check(uint64_t *hashes) const {
    // Prefetch memory locations
    static_assert(std::is_pod<prefetch_info>::value, "prefetch_info must be a POD");
    prefetch_info pinfo[k_];
    const size_t base = d_.remainder(hashes[0]);
    const size_t inc  = d_.remainder(hashes[1]);
    for(unsigned long i = 0; i < k_; ++i) {
      const size_t p   = d_.remainder(base + i * inc);
      const size_t off = p / 5;
      pinfo[i].boff    = p % 5;
      pinfo[i].pos     = data_ + off;
      //      prefetch_read_no(pinfo[i].pos);
      __builtin_prefetch(pinfo[i].pos, 0, 0);
    }

    // Check element
    unsigned char res = 2;
    for(unsigned long i = 0; i < k_; ++i) {
      size_t        boff = pinfo[i].boff;
      unsigned char w    = jflib::a_load(pinfo[i].pos);

      switch(boff) {
      case 0:          break;
      case 1: w /= 3;  break;
      case 2: w /= 9;  break;
      case 3: w /= 27; break;
      case 4: w /= 81; break;
      }
      w = w % 3;
      if(w < res)
        res = w;
    }
    return res;
  }

//   uint64_t dumb_sum() const {
//     uint64_t res = 0;
//     uint64_t *ptr = (uint64_t*)data_;

//     for(size_t i = 0; i < nb_bytes(m()) / 8; ++i)
//       res ^= ptr[i];

//     return res;
//   }

// #define massert(expr) { if(!(expr)) { throw std::runtime_error("error at " + std::to_string(i)); } } while(false);

//   void all_2s() const {
//     for(size_t i = 0; i < nb_bytes(m()); ++i) {
//       unsigned char w = data_[i];
//       massert( w       % 3 != 1);
//       massert((w / 3)  % 3 != 1);
//       massert((w / 9)  % 3 != 1);
//       massert((w / 27) % 3 != 1);
//       massert((w / 81) % 3 != 1);
//     }
//   }

  // Limited std::map interface compatibility
  class element_proxy {
    bloom_counter2_base& bc_;
    const Key&      k_;

  public:
    element_proxy(bloom_counter2_base& bc, const Key& k) : bc_(bc), k_(k) { }

    unsigned int operator++() {
      unsigned int res = bc_.insert(k_);
      return res == 0 ? 1 : 2;
    }

    unsigned int operator++(int) { return bc_.insert(k_); }
    unsigned int operator*() const { return bc_.check(k_); }
    operator unsigned int() const { return bc_.check(k_); }
  };

  class const_element_proxy {
    const bloom_counter2_base& bc_;
    const Key&            k_;

  public:
    const_element_proxy(const bloom_counter2_base& bc, const Key& k) : bc_(bc), k_(k) { }

    unsigned int operator*() const { return bc_.check(k_); }
    operator unsigned int() const { return bc_.check(k_); }
  };

  element_proxy operator[](const Key& k) { return element_proxy(*this, k); }
  const_element_proxy operator[](const Key& k) const { return const_element_proxy(*this, k); }
};

template<typename Key, typename HashPair = hash_pair<Key>, typename atomic_t = ::atomic::gcc,
         typename mem_block_t = allocators::mmap>
class bloom_counter2:
    protected mem_block_t,
    public bloom_counter2_base<Key, HashPair, atomic_t>
{
  typedef bloom_counter2_base<Key, HashPair, atomic_t> super;
  static const double LOG2;
  static const double LOG2_SQ;

  static size_t opt_m(const double fp, const size_t n) {
    return n * (size_t)lrint(-log(fp) / LOG2_SQ);
  }
  static unsigned long opt_k(const double fp) {
    return lrint(-log(fp) / LOG2);
  }

public:
  typedef typename super::key_type key_type;

  bloom_counter2(const double fp, const size_t n, const HashPair& fns = HashPair()) :
    mem_block_t(super::nb_bytes(opt_m(fp, n))),
    super(opt_m(fp, n), opt_k(fp), (unsigned char*)mem_block_t::get_ptr(), fns)
  {
    if(!mem_block_t::get_ptr())
      throw std::runtime_error("Failed to allocate " + std::to_string(super::nb_bytes(opt_m(fp, n))) + " bytes of memory for bloom_counter");
  }

  bloom_counter2(size_t m, unsigned long k, const HashPair& fns = HashPair()) :
    mem_block_t(super::nb_bytes(m)),
    super(m, k, (unsigned char*)mem_block_t::get_ptr(), fns)
  {
    if(!mem_block_t::get_ptr())
      throw std::runtime_error("Failed to allocate " + std::to_string(super::nb_bytes(m)) + " bytes of memory for bloom_counter");
  }

  bloom_counter2(size_t m, unsigned long k, std::istream& is, const HashPair& fns = HashPair()) :
    mem_block_t(super::nb_bytes(m)),
    super(m, k, (unsigned char*)mem_block_t::get_ptr(), fns)
  {
    if(!mem_block_t::get_ptr())
      throw std::runtime_error("Failed to allocate " + std::to_string(super::nb_bytes(m)) + " bytes of memory for bloom_counter");

    is.read((char*)mem_block_t::get_ptr(), mem_block_t::get_size());
  }

  bloom_counter2(const bloom_counter2& rhs) = delete;
  bloom_counter2(bloom_counter2&& rhs) :
    mem_block_t(std::move(rhs)),
    super(std::move(rhs))
  { }
};

template<typename Key, typename HashPair, typename atomic_t, typename mem_block_t>
const double bloom_counter2<Key, HashPair, atomic_t, mem_block_t>::LOG2 = 0.6931471805599453;
template<typename Key, typename HashPair, typename atomic_t, typename mem_block_t>
const double bloom_counter2<Key, HashPair, atomic_t, mem_block_t>::LOG2_SQ = 0.4804530139182014;

template<typename Key, typename HashPair = hash_pair<Key>, typename atomic_t = ::atomic::gcc>
class bloom_counter2_file :
    protected mapped_file,
    public bloom_counter2_base<Key, HashPair, atomic_t>
{
  typedef bloom_counter2_base<Key, HashPair, atomic_t> super;
public:
  typedef typename super::key_type key_type;

  bloom_counter2_file(size_t m, unsigned long k, const char* path, const HashPair& fns = HashPair(), off_t offset = 0) :
    mapped_file(path),
    super(m, k, (unsigned char*)mapped_file::base() + offset, fns)
  { }

  bloom_counter2_file(const bloom_counter2_file& rhs) = delete;
  bloom_counter2_file(bloom_counter2_file&& rhs) :
    mapped_file(std::move(rhs)),
    super(std::move(rhs))
  { }
};

} // namespace jellyfish {

#endif // __BLOOM_COUNTER2_HPP__
