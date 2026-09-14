#include "switch_application.h"

#include <switch.h>

#include <cstddef>

u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = static_cast<size_t>(RPCS3_SWITCH_HEAP_SIZE_MB) * 1024 * 1024;

int main()
{
	rpcs3::switch_app::application application;
	return application.run();
}
