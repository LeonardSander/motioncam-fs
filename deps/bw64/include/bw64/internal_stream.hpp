#ifndef INTERNAL_STREAM_HPP
#define INTERNAL_STREAM_HPP

#include <iostream>
#include <string>
#include <vector>

namespace bw64 {

class StreamWrapper {
public:
    virtual ~StreamWrapper() {};
    
    virtual void seekp(std::streamoff pos, std::ios::seekdir dir=std::ios_base::beg) = 0;
    virtual void seekg(std::streamoff pos, std::ios::seekdir dir=std::ios_base::beg) = 0;
    virtual std::streampos tellp() = 0;
    virtual std::streampos tellg() = 0;
    virtual std::streampos write(const void* data, size_t len) = 0;
    virtual std::streampos write(const std::vector<char>& data) = 0;
    virtual std::streampos read(void* dst, size_t len) = 0;
    virtual void clear() = 0;
    virtual bool good() = 0;
    virtual bool eof() = 0;
};

class MemoryStreamWrapper : public StreamWrapper {
public:
    MemoryStreamWrapper(std::vector<uint8_t>& output) : data(output), offset(0) {
        output.resize(0);
    }
    
    void seekp(std::streamoff pos, std::ios::seekdir dir=std::ios_base::beg) {
        if(dir == std::ios_base::beg)
            offset = pos;
        else if(dir == std::ios_base::cur)
            offset += pos;
        else if(dir == std::ios_base::end)
            offset = data.size() + pos;
    }
    
    void seekg(std::streamoff pos, std::ios::seekdir dir=std::ios_base::beg) {
        throw std::runtime_error("Unsupported operation");
    }
    
    std::streampos tellp() {
        return offset;
    }
    
    std::streampos tellg() {
        throw std::runtime_error("Unsupported operation");
    }
    
    std::streampos write(const void* buf, size_t len) {
        if(offset == data.size()) {
            data.insert(data.begin()+offset, reinterpret_cast<const uint8_t*>(buf), reinterpret_cast<const uint8_t*>(buf) + len);
        }
        else {
            std::streamoff streamLen = len;
            
            if(offset + streamLen > data.size())
                data.resize(offset + streamLen);
            
            std::copy(reinterpret_cast<const uint8_t*>(buf), reinterpret_cast<const uint8_t*>(buf)+len, data.data() + offset);
        }
        
        offset += len;
        
        return len;
    }
    
    std::streampos write(const std::vector<char>& buf) {
        if(offset == data.size()) {
            data.insert(data.begin()+offset, buf.begin(), buf.end());
        }
        else {
            std::streamoff streamLen = buf.size();
            
            if(offset + streamLen > data.size())
                data.resize(offset + streamLen);
            
            std::copy(buf.begin(), buf.end(), data.begin() + offset);
        }

        offset += buf.size();
        
        return buf.size();
    }
    
    std::streampos read(void* dst, size_t len) {
        throw std::runtime_error("Unsupported operation");
    }
    
    void clear() {
    }
    
    bool good() {
        return true;
    }
    
    bool eof() {
        return false;
    }
    
private:
    std::vector<uint8_t>& data;
    std::streampos offset;
};

class FileStreamWrapper : public StreamWrapper {
public:
    FileStreamWrapper(const std::string& path, bool write=true) {
        mFile = fopen(path.c_str(), write ? "wb" : "rb");
        if(!mFile)
            throw std::runtime_error("Failed to open file");
    }

    FileStreamWrapper(const int fd, bool write=true) {
        mFile = fdopen(fd, write ? "w" : "r");
        if(!mFile)
            throw std::runtime_error("Failed to open fd");
    }
    
    ~FileStreamWrapper() {
        if(mFile)
            fclose(mFile);
        mFile = nullptr;
    }
    
    void seekp(std::streamoff pos, std::ios::seekdir dir=std::ios_base::beg) {
        int origin = SEEK_SET;
        if(dir == std::ios_base::beg)
            origin = SEEK_SET;
        else if(dir == std::ios_base::cur)
            origin = SEEK_CUR;
        else if(dir == std::ios_base::end)
            origin = SEEK_END;

        fseek(mFile, pos, origin);
    }

    void seekg(std::streamoff pos, std::ios::seekdir dir=std::ios_base::beg) {
        seekp(pos, dir);
    }
    
    std::streampos tellp() {
        return ftell(mFile);
    }

    std::streampos tellg() {
        return ftell(mFile);
    }

    std::streampos write(const void* data, size_t len) {
        return fwrite(data, 1, len, mFile);
    }
    
    std::streampos write(const std::vector<char>& data) {
        return fwrite(data.data(), 1, data.size(), mFile);
    }
    
    std::streampos read(void* dst, size_t len) {
        return fread(dst, 1, len, mFile);
    }
    
    void clear() {
        clearerr(mFile);
    }
    
    bool good() {
        return ferror(mFile) == 0;
    }
    
    bool eof() {
        return feof(mFile) != 0;
    }

private:
    FILE* mFile;
};

}

#endif /*INTERNAL_STREAM_HPP*/
