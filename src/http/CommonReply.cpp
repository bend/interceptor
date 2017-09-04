#include "CommonReply.h"

#include "HttpException.h"

#include "Request.h"
#include "Headers.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include "common/Buffer.h"

#include <regex>
#include <boost/algorithm/string/replace.hpp>

namespace Interceptor::Http {

  CommonReply::CommonReply(HttpRequestPtr request, const SiteConfig* config)
    : m_request(request),
      m_replyHeaders(std::make_unique<Headers>()),
      m_httpBuffer(std::make_shared<Buffer>()),
      m_config(config),
      m_gzipBusy(false),
      m_status(Code::Ok)
  {
  }

  Code CommonReply::getLocationCode(const SiteConfig* site) const
  {
    std::string idx = m_request->index();

    for (const auto& kv : site->m_locations) {
      std::regex reg(kv.first, std::regex_constants::ECMAScript);

      if (std::regex_search(idx, reg) > 0) {
        return (Code)kv.second;
      }
    }

    return Code::Ok;
  }

  void CommonReply::setFlag(Flag flag, bool value)
  {
    m_flags.set(flag, value);
  }

  bool CommonReply::getFlag(Flag flag) const
  {
    return m_flags.test(flag);
  }


  void CommonReply::requestFileContents(const std::string& page,
                                        std::stringstream& stream, size_t bytes)
  {
    Code ret = m_request->cacheHandler()->read(page, stream, bytes);

    if (ret == Code::Ok) {
      setHeadersFor(page);
      m_request->setCompleted(true);
      m_contentLength = bytes;
      //post(stream);
    } else {
      throw HttpException(ret, false);
    }
  }

  void CommonReply::requestPartialFileContents(const std::string& page,
      std::stringstream& stream, size_t& bytes)
  {
    LOG_DEBUG("CommonReply::requestPartialFileContents()");
    std::tuple<int64_t, int64_t> range;
    Code ret;

    if ((ret = m_request->getRangeRequest(range)) != Code::Ok) {
      throw HttpException(ret, true);
    }

    int64_t from = std::get<0>(range);
    int64_t to = std::get<1>(range);
    size_t total = 0;
    ret = FileUtils::calculateBounds(page, from, to);

    if (ret != Code::Ok) {
      throw HttpException(ret, true);
    }

    if (!FileUtils::fileSize(page, total)) {
      throw HttpException(Code::NotFound, false);
    }

    bytes = to - from + 1;
    m_contentLength = bytes;

    m_replyHeaders->addHeader("Content-Range",
                              "bytes " + std::to_string(from) + "-"
                              + std::to_string(to) + "/" + std::to_string(total));
    m_status = Code::PartialContent;
    setHeadersFor(page);

    if (to - from > MAX_CHUNK_SIZE) {
      // File is too big to be sent at once, we will send it in multiple times to
      // avoid consuming to much memory
      if (!requestLargeFileContents(page, stream, from, to, total)) {
        throw HttpException(Code::InternalServerError, true);
      }
    } else {
      ret = FileUtils::readFile(page,	from, to , stream,
                                bytes); //TODO take it from cache

      if (ret == Code::Ok) {
        m_request->setCompleted(true);
      } else {
        throw HttpException(ret, true);
      }
    }
  }

  BufferPtr CommonReply::requestFileChunk(const std::string& page, size_t from,
                                          size_t limit, size_t& bytes)
  {
    std::stringstream stream;

    if (requestLargeFileContents(page, stream, from, limit, bytes)) {
      serialize(stream);
    }

    return m_httpBuffer;
  }


  bool CommonReply::requestLargeFileContents(const std::string& page,
      std::stringstream& stream, size_t from,
      size_t limit,
      size_t totalBytes)
  {
    LOG_DEBUG("CommonReply::requestLargeFileContents()");
    //needed to be sure that previous call is completed
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t bytes;
    size_t to = std::min(limit, std::min((size_t) from + MAX_CHUNK_SIZE,
                                         totalBytes - 1));
    setFlag(LargeFileRequest, true);

    m_httpBuffer = std::make_shared<Buffer>();

    if (FileUtils::readFile(page, from, to, stream, bytes) == Code::Ok) {
      if (to == limit - 1) {
        m_request->setCompleted(true);
      } else {
        from = to + 1;
        m_httpBuffer->m_nextCall = std::bind(&CommonReply::requestFileChunk,
                                             shared_from_this(), page, from , limit, totalBytes);
        m_httpBuffer->m_flags |= Buffer::HasMore;
      }

    } else {
      return false;
    }

    return true;
  }


  std::string CommonReply::requestedPath() const
  {
    std::string page;

    if ( isRequestingRoot()) {
      page = getRootFile();
    } else {
      // This request contains the filename, hence we should
      // not try a filename from the list of try-files
      page = m_config->m_docroot + m_request->index();

      if (!FileUtils::exists(page)) {
        throw HttpException(Code::NotFound);
      }
    }

    // page found
    boost::algorithm::replace_all(page, "///", "/");
    boost::algorithm::replace_all(page, "//", "/");
    return page;
  }

  bool CommonReply::isRequestingRoot() const
  {
    return  m_request->index() == ""
            || m_request->index() == "/"
            || m_request->index().at(m_request->index().length() - 1) == '/';
  }

  std::string CommonReply::getRootFile() const
  {
    std::string page;
    bool found = false;
    std::vector<std::string> tryFiles = m_config->m_tryFiles;

    for (const auto& index : tryFiles) {
      page = m_config->m_docroot + m_request->index() + "/" + index;

      if (FileUtils::exists(page)) {
        found = true;
        break;
      }
    }

    if (!found) {
      throw HttpException(Code::NotFound);
    }

    return page;
  }

  void CommonReply::setHeadersFor(const std::string& filename)
  {

    m_replyHeaders->setHeadersFor(filename, m_request->cacheHandler());

    if (!m_request->hasMatchingSite()) {
      setFlag(Flag::GzipEncoding, false);
      return;
    }

    auto site = m_request->matchingSite();

    if (site->m_gzip.count("all") == 0
        && site->m_gzip.count(FileUtils::extension(filename)) == 0) {
      setFlag(Flag::GzipEncoding, false);
    }

  }

  bool CommonReply::chunkResponse(BufferPtr httpBuffer,
                                  std::vector<boost::asio::const_buffer>& buffers)
  {
    LOG_DEBUG("CommonReply::chunkResponse()");
    size_t size = 0;

    // We cannot take the total Content Length here because the gzip compression changed that
    // Length, so we need to recalculate it
    for (auto& buffer : buffers) {
      size += boost::asio::buffer_size(buffer);
    }

    std::stringstream stream;
    stream << std::hex << size << "\r\n";

    char* header = new char[stream.str().length()]();
    memcpy(header, stream.str().data(), stream.str().length());

    buffers.insert(buffers.begin(), m_httpBuffer->buf(header,
                   stream.str().length()));

    stream.str("\r\n");
    char* crlf = new char[stream.str().length()]();
    memcpy(crlf, stream.str().data(), stream.str().length());
    buffers.push_back(m_httpBuffer->buf(crlf, stream.str().length()));

    if (!getFlag(LargeFileRequest) || m_request->completed()) {
      stream.str("0\r\n\r\n");
      char* footer = new char[stream.str().length()]();
      memcpy(footer, stream.str().data(), stream.str().length());
      buffers.push_back(m_httpBuffer->buf(footer, stream.str().length()));
    }

    return true;
  }

#ifdef ENABLE_GZIP
  bool CommonReply::encodeResponse(BufferPtr httpBuffer,
                                   std::vector<boost::asio::const_buffer>& buffers)
  {
    LOG_DEBUG("CommonReply::encodeResponse()");
    std::vector<boost::asio::const_buffer> result;

    if (!m_gzipBusy) {
      initGzip();
    }

    unsigned int i = 0;

    for (auto& buffer : buffers) {

      m_gzip.avail_in = boost::asio::buffer_size(buffer);
      m_gzip.next_in = (unsigned char*) boost::asio::detail::buffer_cast_helper(
                         buffer);

      char out[16 * 1024];

      do {
        m_gzip.next_out = (unsigned char*)out;
        m_gzip.avail_out = sizeof(out);

        int res = 0;
        res = deflate(&m_gzip,
                      (i == buffers.size() - 1 && m_request->completed()) ?
                      Z_FINISH : Z_NO_FLUSH);
        assert(res != Z_STREAM_ERROR);

        unsigned have = sizeof(out) - m_gzip.avail_out;
        m_contentLength += have;

        if (have) {
          result.push_back(httpBuffer->buf(std::string((char*)out, have)));
        }
      } while (m_gzip.avail_out == 0);

      ++i;

    }

    if (m_request->completed()) {
      deflateEnd(&m_gzip);
      m_gzipBusy = false;
    }

    buffers.clear();
    buffers.insert(buffers.begin(), result.begin(), result.end());

    return true;
  }

  bool CommonReply::canChunkResponse() const
  {
    return getFlag(Flag::ChunkedEncoding)
           && m_request->method() != Method::HEAD
           && m_status != Code::PartialContent;
  }

  bool CommonReply::canEncodeResponse() const
  {
#ifdef ENABLE_GZIP
    return getFlag(Flag::GzipEncoding) && m_request->method() != Method::HEAD
           && m_status != Code::PartialContent;
#else
    return false;
#endif // ENABLE_GZIP
  }


  void CommonReply::initGzip()
  {
    LOG_DEBUG("CommonReply::initGzip()");
    m_gzip.zalloc = Z_NULL;
    m_gzip.zfree = Z_NULL;
    m_gzip.opaque = Z_NULL;
    m_gzip.next_in = Z_NULL;
    int r = 0;

    r = deflateInit2(&m_gzip, Z_DEFAULT_COMPRESSION,
                     Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);

    m_gzipBusy = true;
    assert(r == Z_OK);
  }
#endif // ENABLE_GZIP

  void CommonReply::buildHeaders(BufferPtr httpBuffer)
  {
    LOG_DEBUG("CommonReply::buildHeaders()");
    std::stringstream stream;
    stream << "HTTP/" << m_request->httpVersion() << " ";
    stringValue(m_status, stream);

    if (canChunkResponse()) {
      m_replyHeaders->addHeader("Transfer-Encoding", "chunked");
    } else {
      m_replyHeaders->addHeader("Content-Length", m_contentLength);
    }

    if (canEncodeResponse()) {
      LOG_DEBUG("Content-Encoding: gzip");
      m_replyHeaders->addHeader("Content-Encoding", "gzip");
    }

    if (getFlag(Flag::Closing)) {
      m_replyHeaders->addHeader("Connection", "close");
      httpBuffer->m_flags |= Buffer::Closing;
    } else {
      m_replyHeaders->addHeader("Connection", "keep-alive");
    }

    m_replyHeaders->serialize(stream);
    const std::string& resp = stream.str();

    if (httpBuffer->m_buffers.size() == 0) {
      httpBuffer->m_buffers.push_back(httpBuffer->buf(std::string(resp)));
    } else {
      httpBuffer->m_buffers[0] = httpBuffer->buf(std::string(resp));
    }
  }

}