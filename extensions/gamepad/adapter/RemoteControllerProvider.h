// SPDX-License-Identifier: MPL-2.0 OR GPL-3.0-only
#pragma once
#include "input/api/ControllerProvider.h"

class RemoteGamePadProvider final : public ControllerProviderBase
{
public:
    InputAPI::Type api() const override { return InputAPI::RemoteGamePad; }
    std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;
};
