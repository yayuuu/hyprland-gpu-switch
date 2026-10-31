#pragma once
#include <functional>
#include <string>
#include "diagnostics/diagnostics.hpp"
namespace GpuSwitch {
#if GPU_SWITCH_DIAGNOSTICS
std::string bridgeStatus();
#endif
void initMigration(void* handle);
void exitMigration();
void cancelMigration();
void maintainMigration();
std::string migrate(const std::string& deviceId,const std::function<bool()>& validate);
std::string migrationReadiness();
std::string renderingDevice();
std::string renderingName();
bool trackingComplete();
bool migrationPaused();
bool migrationIdleReady();
}
