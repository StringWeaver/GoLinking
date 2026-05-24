#pragma once
#include "Class.g.h"

namespace winrt::SingBox_Task::implementation
{
    struct Class : ClassT<Class>
    {
        Class() = default;

        void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance const& taskInstance);
    };
}

namespace winrt::SingBox_Task::factory_implementation
{
    struct Class : ClassT<Class, implementation::Class>
    {
    };
}
