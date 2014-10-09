// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * LGPL2.1 (see COPYING-LGPL2.1) or later
 */

#ifndef BIT_VECTOR_HPP
#define BIT_VECTOR_HPP

#include "common/Formatter.h"
#include "include/assert.h"
#include "include/buffer.h"
#include "include/encoding.h"
#include <stdint.h>
#include <cmath>
#include <list>
#include <boost/static_assert.hpp>

namespace ceph {

template <uint8_t _bit_count>
class BitVector
{
private:
  static const uint8_t BITS_PER_BYTE = 8;
  static const uint32_t ELEMENTS_PER_BLOCK = BITS_PER_BYTE / _bit_count;
  static const uint8_t MASK = static_cast<uint8_t>((1 << _bit_count) - 1);

  // must be power of 2
  BOOST_STATIC_ASSERT((_bit_count != 0) && !(_bit_count & (_bit_count - 1)));
  BOOST_STATIC_ASSERT(_bit_count <= BITS_PER_BYTE);
public:

  class ConstReference {
  public:
    operator uint8_t() const;
  private:
    friend class BitVector;
    const BitVector &m_bit_vector;
    size_t m_offset;

    ConstReference(const BitVector &bit_vector, size_t offset);
  };

  class Reference {
  public:
    operator uint8_t() const;
    Reference& operator=(uint8_t v);
  private:
    friend class BitVector;
    BitVector &m_bit_vector;
    size_t m_offset;

    Reference(BitVector &bit_vector, size_t offset);
  };

  static const uint8_t BIT_COUNT = _bit_count;

  BitVector();

  void clear();

  void resize(size_t elements);
  size_t size() const;

  const bufferlist& get_data() const;
  void get_data_extents(size_t offset, size_t length,
		        size_t *byte_offset, size_t *byte_length) const;
  size_t get_data_buffer_offset() const;

  Reference operator[](size_t offset);
  ConstReference operator[](size_t offset) const;

  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);
  void dump(Formatter *f) const;

  bool operator==(const BitVector &b) const;

  static void generate_test_instances(std::list<BitVector *> &o);
private:

  bufferlist m_data;
  size_t m_size;

  static void compute_index(size_t offset, size_t *index, size_t *shift);

};

template <uint8_t _b>
BitVector<_b>::BitVector() : m_size(0)
{
}

template <uint8_t _b>
void BitVector<_b>::clear() {
  m_data.clear();
  m_size = 0;
}

template <uint8_t _b>
void BitVector<_b>::resize(size_t size) {
  size_t buffer_size = static_cast<size_t>(std::ceil(static_cast<double>(size) /
		       		    	   ELEMENTS_PER_BLOCK));
  if (buffer_size > m_data.length()) {
    m_data.append_zero(buffer_size - m_data.length());
  } else if (buffer_size < m_data.length()) {
    bufferlist bl;
    bl.substr_of(m_data, 0, buffer_size);
    bl.swap(m_data);
  }
  m_size = size;
}

template <uint8_t _b>
size_t BitVector<_b>::size() const {
  return m_size;
}

template <uint8_t _b>
const bufferlist& BitVector<_b>::get_data() const {
  return m_data;
}

template <uint8_t _b>
void BitVector<_b>::compute_index(size_t offset, size_t *index, size_t *shift) {
  *index = offset / ELEMENTS_PER_BLOCK;
  *shift = ((ELEMENTS_PER_BLOCK - 1) - (offset % ELEMENTS_PER_BLOCK)) * _b;
}

template <uint8_t _b>
void BitVector<_b>::get_data_extents(size_t offset, size_t length,
				     size_t *byte_offset,
				     size_t *byte_length) const {
  assert(length > 0);
  size_t end_offset, shift;
  compute_index(offset, byte_offset, &shift);
  compute_index(offset + length - 1, &end_offset, &shift);
  *byte_length = end_offset - *byte_offset + 1;
}

template <uint8_t _b>
size_t BitVector<_b>::get_data_buffer_offset() const {
  // 6 byte encoding header, 8 byte size, 4 byte bufferlist length
  return 18;
}

template <uint8_t _b>
void BitVector<_b>::encode(bufferlist& bl) const {
  ENCODE_START(1, 1, bl);
  ::encode(m_size, bl);
  ::encode(m_data, bl);
  ENCODE_FINISH(bl);
}

template <uint8_t _b>
void BitVector<_b>::decode(bufferlist::iterator& bl) {
  DECODE_START(1, bl);
  ::decode(m_size, bl);
  ::decode(m_data, bl);
  DECODE_FINISH(bl);
}

template <uint8_t _b>
void BitVector<_b>::dump(Formatter *f) const {
  f->dump_unsigned("size", m_size);
  f->open_array_section("bit_table");
  for (unsigned i = 0; i < m_data.length(); ++i) {
    f->dump_format("byte", "0x%02hhX", m_data[i]);
  }
  f->close_section();
}

template <uint8_t _b>
bool BitVector<_b>::operator==(const BitVector &b) const {
  return (this->m_size == b.m_size && this->m_data == b.m_data);
}

template <uint8_t _b>
typename BitVector<_b>::Reference BitVector<_b>::operator[](size_t offset) {
  return Reference(*this, offset);
}

template <uint8_t _b>
typename BitVector<_b>::ConstReference BitVector<_b>::operator[](size_t offset) const {
  return ConstReference(*this, offset);
}

template <uint8_t _b>
BitVector<_b>::ConstReference::ConstReference(const BitVector<_b> &bit_vector,
					      size_t offset)
  : m_bit_vector(bit_vector), m_offset(offset)
{
}

template <uint8_t _b>
BitVector<_b>::ConstReference::operator uint8_t() const {
  size_t index;
  size_t shift;
  this->m_bit_vector.compute_index(this->m_offset, &index, &shift);

  return (this->m_bit_vector.m_data[index] >> shift) & MASK;
}

template <uint8_t _b>
BitVector<_b>::Reference::Reference(BitVector<_b> &bit_vector, size_t offset)
  : m_bit_vector(bit_vector), m_offset(offset)
{
}

template <uint8_t _b>
BitVector<_b>::Reference::operator uint8_t() const {
  size_t index;
  size_t shift;
  this->m_bit_vector.compute_index(this->m_offset, &index, &shift);

  return (this->m_bit_vector.m_data[index] >> shift) & MASK;
}

template <uint8_t _b>
typename BitVector<_b>::Reference& BitVector<_b>::Reference::operator=(uint8_t v) {
  size_t index;
  size_t shift;
  this->m_bit_vector.compute_index(this->m_offset, &index, &shift);

  // TODO: find out why bufferlist doesn't support char& operator[]()
  uint8_t mask = MASK << shift;
  char* packed_data = this->m_bit_vector.m_data.c_str();
  uint8_t packed_value = (packed_data[index] & ~mask) | ((v << shift) & mask);
  packed_data[index] = packed_value;
  return *this;
}

template <uint8_t _b>
void BitVector<_b>::generate_test_instances(std::list<BitVector *> &o) {
  o.push_back(new BitVector());

  BitVector *b = new BitVector();
  const uint64_t radix = 1 << b->BIT_COUNT;
  const size_t size = 1024;

  b->resize(size);
  for (size_t i = 0; i < size; ++i) {
    (*b)[i] = rand() % radix;
  }
  o.push_back(b);
}

}

WRITE_CLASS_ENCODER(ceph::BitVector<2>)

template <uint8_t _b>
inline std::ostream& operator<<(std::ostream& out, const ceph::BitVector<_b> &b)
{
  out << "ceph::BitVector<" << _b << ">(size=" << b.size() << ", data="
      << b.get_data() << ")";
  return out;
}

#endif // BIT_VECTOR_HPP
