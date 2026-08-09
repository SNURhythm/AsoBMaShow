#if defined(_MSC_VER)

#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>

namespace {

class MsvcTestDiagnostics {
public:
  MsvcTestDiagnostics() noexcept {
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS |
                 SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    constexpr int reportTypes[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (const int reportType : reportTypes) {
      _CrtSetReportMode(reportType, _CRTDBG_MODE_FILE);
      _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(_WRITE_ABORT_MSG,
                        _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  }
};

const MsvcTestDiagnostics diagnostics;

} // namespace

#endif
