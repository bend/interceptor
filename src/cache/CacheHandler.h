#ifndef CACHE_HANDLER_H__
#define CACHE_HANDLER_H__

#include <string>
#include "AbstractCacheHandler.h"

class CacheHandler : public AbstractCacheHandler {
public:
  CacheHandler(size_t maxCacheSize);

  virtual std::string eTag(const std::string& file) override;

  virtual std::string lastModified(const std::string& file) override;

  virtual bool size(const std::string& file, size_t& bytes) override;

  virtual Http::Code read(const std::string& file, std::stringstream& out,
                          size_t& bytes) override;

protected:
  virtual size_t cacheSize() const override;
};

#endif // ABSTRACT_CACHE_HANDLER_H__
