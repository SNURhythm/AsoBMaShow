#pragma once

// Skin-facing Beatoraja IDs stay stable: 2 is assist, 3 is light assist.
constexpr int musicSelectLampOrder(int lamp) {
  return lamp == 3 ? 2 : lamp == 2 ? 3 : lamp;
}
