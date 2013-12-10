// Copyright 2010-2013 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_MSC_VER)
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

#include <string>

#include "base/file.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"

namespace operations_research {

File::File(FILE* const f_des, const std::string& name)
  : f_(f_des), name_(name) {}

bool File::Delete(const char* const name) {
  return remove(name) == 0;
}

bool File::Exists(const char* const name) {
  return access(name, F_OK) == 0;
}

size_t File::Size() {
  struct stat f_stat;
  stat(name_.c_str(), &f_stat);
  return f_stat.st_size;
}

bool File::Flush() {
  return fflush(f_) == 0;
}

bool File::Close() {
  if (fclose(f_) == 0) {
    f_ = NULL;
    return true;
  } else {
    return false;
  }
}

void File::ReadOrDie(void* const buf, size_t size) {
  CHECK_EQ(fread(buf, 1, size, f_), size);
}

size_t File::Read(void* const buf, size_t size) {
  return fread(buf, 1, size, f_);
}

void File::WriteOrDie(const void* const buf, size_t size) {
  CHECK_EQ(fwrite(buf, 1, size, f_), size);
}
size_t File::Write(const void* const buf, size_t size) {
  return fwrite(buf, 1, size, f_);
}

File* File::OpenOrDie(const char* const name, const char* const flag) {
  FILE* const f_des = fopen(name, flag);
  if (f_des == NULL) {
    std::cerr << "Cannot open " << name;
    exit(1);
  }
  File* const f = new File(f_des, name);
  return f;
}

File* File::Open(const char* const name, const char* const flag) {
  FILE* const f_des = fopen(name, flag);
  if (f_des == NULL) return NULL;
  File* const f = new File(f_des, name);
  return f;
}

char* File::ReadLine(char* const output, uint64 max_length) {
  return fgets(output, max_length, f_);
}

int64 File::ReadToString(std::string* const output, uint64 max_length) {
  CHECK_NOTNULL(output);
  output->clear();

  if (max_length == 0) return 0;
  if (max_length < 0) return -1;

  int64 needed = max_length;
  int bufsize = (needed < (2 << 20) ? needed : (2 << 20));

  scoped_ptr<char[]> buf(new char[bufsize]);

  int64 nread = 0;
  while (needed > 0) {
    nread = Read(buf.get(), (bufsize < needed ? bufsize : needed));
    if (nread > 0) {
      output->append(buf.get(), nread);
      needed -= nread;
    } else {
      break;
    }
  }
  return (nread >= 0 ? static_cast<int64>(output->size()) : -1);
}

size_t File::WriteString(const std::string& line) {
  return Write(line.c_str(), line.size());
}

bool File::WriteLine(const std::string& line) {
  if (Write(line.c_str(), line.size()) != line.size()) return false;
  return Write("\n", 1) == 1;
}

std::string File::filename() const {
  return name_;
}

bool File::Open() const {
  return f_ != NULL;
}

void File::Init() {}

namespace file {
Status SetContents(const std::string& filename, const std::string& contents,
                   int flags) {
  if (flags != Defaults()) {
    LOG(DFATAL) << "file::SetContents() with unsupported flags=" << flags;
    return Status(false);
  }
  File* file = File::Open(filename, "w");
  if (file == NULL) return Status(false);
  return Status(file->WriteString(contents) == contents.size());
}

Status GetContents(const std::string& filename, std::string* output,
                   int flags) {
  if (flags != Defaults()) {
    LOG(DFATAL) << "file::GetContents() with unsupported flags=" << flags;
    return Status(false);
  }
  File* file = File::Open(filename, "r");
  if (file == NULL) return Status(false);
  int64 size = file->Size();
  return Status(size == file->ReadToString(output, size));
}

bool ReadFileToString(const string& file_name, string* output) {
  return GetContents(file_name, output, file::Defaults()).ok();
}

bool WriteStringToFile(const string& data, const string& file_name) {
  return SetContents(file_name, data, file::Defaults()).ok();
}

namespace {
class NoOpErrorCollector : public google::protobuf::io::ErrorCollector {
 public:
  virtual void AddError(int line, int column, const string& message) { }
};
}  // namespace

bool ReadFileToProto(const string& file_name, google::protobuf::Message* proto) {
  string str;
  if (!ReadFileToString(file_name, &str)) {
    LOG(INFO) << "Could not read " << file_name;
    return false;
  }
  // Attempt to decode ASCII before deciding binary. Do it in this order because
  // it is much harder for a binary encoding to happen to be a valid ASCII
  // encoding than the other way around. For instance "index: 1\n" is a valid
  // (but nonsensical) binary encoding. We want to avoid printing errors for
  // valid binary encodings if the ASCII parsing fails, and so specify a no-op
  // error collector.
  NoOpErrorCollector error_collector;
  google::protobuf::TextFormat::Parser parser;
  parser.RecordErrorsTo(&error_collector);
  if (parser.ParseFromString(str, proto)) {
    return true;
  }
  if (proto->ParseFromString(str)) {
    return true;
  }
  // Re-parse the ASCII, just to show the diagnostics (we could also get them
  // out of the ErrorCollector but this way is easier).
  google::protobuf::TextFormat::ParseFromString(str, proto);
  LOG(INFO) << "Could not parse contents of " << file_name;
  return false;
}

void ReadFileToProtoOrDie(const string& file_name, google::protobuf::Message* proto) {
  CHECK(ReadFileToProto(file_name, proto)) << "file_name: " << file_name;
}

bool WriteProtoToASCIIFile(const google::protobuf::Message& proto,
                           const string& file_name) {
  string proto_string;
  return google::protobuf::TextFormat::PrintToString(proto, &proto_string) &&
         WriteStringToFile(proto_string, file_name);
}

void WriteProtoToASCIIFileOrDie(const google::protobuf::Message& proto,
                                const string& file_name) {
  CHECK(WriteProtoToASCIIFile(proto, file_name)) << "file_name: " << file_name;
}

bool WriteProtoToFile(const google::protobuf::Message& proto, const string& file_name) {
  string proto_string;
  return proto.AppendToString(&proto_string) &&
         WriteStringToFile(proto_string, file_name);
}

void WriteProtoToFileOrDie(const google::protobuf::Message& proto,
                           const string& file_name) {
  CHECK(WriteProtoToFile(proto, file_name)) << "file_name: " << file_name;
}

}  // namespace file

}  // namespace operations_research
