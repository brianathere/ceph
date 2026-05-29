#pragma once
// Test-only shim: no system boost here, and dispatch_io.cc only needs
// small_vector's std::vector-compatible surface (iterator ctor, size(),
// operator[], begin/end). Back it with std::vector.
#include <vector>
#include <cstddef>

namespace boost {
namespace container {
template <class T, std::size_t N>
class small_vector : public std::vector<T> {
public:
  using std::vector<T>::vector;  // inherit ctors incl. (first,last) iterator ctor
};
}  // namespace container
}  // namespace boost
