// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/android_app.h"

namespace RuntimePlatform::Android {
namespace {

// Deliberately a function-local static rather than a namespace-scope object:
// the OpenXR loader bring-up runs from the Android entry point before main(),
// and a namespace-scope AppContext in another translation unit would have an
// unspecified construction order relative to it.
AppContext& Storage() {
    static AppContext context;
    return context;
}

bool& Published() {
    static bool published = false;
    return published;
}

} // namespace

void SetAppContext(const AppContext& context) {
    Storage() = context;
    Published() = true;
}

bool HasAppContext() { return Published(); }

const AppContext& GetAppContext() { return Storage(); }

std::filesystem::path PreferredDataRoot() {
    const AppContext& context = GetAppContext();
    if (!context.external_data_path.empty()) {
        return context.external_data_path;
    }
    return context.internal_data_path;
}

} // namespace RuntimePlatform::Android
