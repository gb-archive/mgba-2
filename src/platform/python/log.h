/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "core/log.h"

struct mLoggerPy {
    struct mLogger d;
    void* pyobj;
};

struct mLogger* mLoggerPythonCreate(void* pyobj);

extern "Python+C" void _pyLog(void* logger, int category, enum mLogLevel level, const char* message);
