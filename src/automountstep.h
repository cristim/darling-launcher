// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "automount.h"

// The real step: runs the existing root-owned mount helpers (which ask polkit for authorization themselves) with the
// fixed arguments AutoMount::commandFor builds, after the trusted-install checks of the mount dialog. Blocking: call it from
// a worker thread or the CLI. It never reads a password and never starts a helper that failed those checks.
class HelperMountStep : public AutoMount::Step {
public:
    AutoMount::StepResult run(const AutoMount::StepRequest &request) override;
};
