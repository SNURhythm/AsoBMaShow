#pragma once

#include "../BmsSearchService.h"

struct FindBmsDialogPolicy {
  bool canDismiss = false;
  bool showCloseOrCancel = false;
  bool showPendingActions = false;
  bool showNormalResultActions = false;
};

FindBmsDialogPolicy findBmsDialogPolicy(bool running,
                                        const BmsSearchResult &result);
