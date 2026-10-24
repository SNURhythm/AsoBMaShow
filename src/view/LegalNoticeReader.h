#pragma once
#include "DocumentTextReader.h"

class LegalNoticeReader : public DocumentTextReader {
public:
  LegalNoticeReader() { setName("legalNoticeReader"); }
  void setNotice(std::string text) { setDocumentText(std::move(text)); }
};
