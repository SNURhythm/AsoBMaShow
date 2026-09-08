#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <utility>
#include <vector>

template <typename T> class BoundedPageCache {
public:
  using PageLoader =
      std::function<std::vector<T>(std::size_t offset, std::size_t limit)>;

  BoundedPageCache() = default;

  BoundedPageCache(std::size_t count, PageLoader loader) {
    reset(count, std::move(loader));
  }

  void reset(std::size_t count, PageLoader loader) {
    releasePages();
    totalCount = count;
    pageLoader = std::move(loader);
  }

  void releasePages() { pages.clear(); }

  void clear() {
    releasePages();
    totalCount = 0;
    pageLoader = {};
  }

  [[nodiscard]] std::size_t size() const { return totalCount; }

  [[nodiscard]] const T &get(std::size_t index) const {
    if (index >= totalCount || !pageLoader) {
      return fallbackRecord;
    }

    const std::size_t pageIndex = index / pageSize;
    const std::size_t localIndex = index % pageSize;
    auto page = std::find_if(pages.begin(), pages.end(),
                             [pageIndex](const Page &candidate) {
                               return candidate.index == pageIndex;
                             });
    if (page == pages.end() || localIndex >= page->records.size()) {
      const std::size_t offset = pageIndex * pageSize;
      auto records = pageLoader(offset, pageSize);
      const std::size_t recordLimit = std::min(pageSize, totalCount - offset);
      if (records.size() > recordLimit) {
        records.resize(recordLimit);
      }
      if (localIndex >= records.size()) {
        return fallbackRecord;
      }
      if (page == pages.end()) {
        pages.push_front(Page{pageIndex, std::move(records)});
        page = pages.begin();
      } else {
        page->records = std::move(records);
      }
    }
    pages.splice(pages.begin(), pages, page);
    while (pages.size() > maxPages) {
      pages.pop_back();
    }
    return page->records[localIndex];
  }

private:
  struct Page {
    std::size_t index;
    std::vector<T> records;
  };

  static constexpr std::size_t pageSize = 128;
  static constexpr std::size_t maxPages = 6;
  std::size_t totalCount = 0;
  PageLoader pageLoader;
  mutable std::list<Page> pages;
  T fallbackRecord{};
};
