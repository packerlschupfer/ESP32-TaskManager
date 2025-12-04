#include "TaskRecovery.h"

// Static member definitions
std::vector<TaskRecovery::RecoveryPolicy> TaskRecovery::recoveryPolicies_;
std::vector<TaskRecovery::TaskRecoveryInfo> TaskRecovery::recoveryInfos_;
SemaphoreHandle_t TaskRecovery::recoveryMutex_ = nullptr;
bool TaskRecovery::initialized_ = false;