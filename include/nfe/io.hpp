#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace nfe {

// Read-only memory map of a whole file. Uncompress the .gz sample first.
class MappedFile {
 public:
  explicit MappedFile(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error("cannot open " + path);
    struct stat st{};
    ::fstat(fd_, &st);
    size_ = size_t(st.st_size);
    if (size_ > 0) {
      void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
      if (p == MAP_FAILED) throw std::runtime_error("mmap failed for " + path);
      ::madvise(p, size_, MADV_SEQUENTIAL);
      data_ = static_cast<const uint8_t*>(p);
    }
  }
  ~MappedFile() {
    if (data_) ::munmap(const_cast<uint8_t*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
  }
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  int fd_ = -1;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

}  // namespace nfe
