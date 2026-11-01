#pragma once
namespace Offload {
// Hidden windows retain their ready transport while its source GPU is already
// needed. Do not keep a disconnected discrete GPU awake just for its worker.
inline bool keepWorkerWarm(bool mapped,bool enabled,bool displayActive,
                           bool integratedSource,bool sourceHasActiveOutput) {
    return mapped && enabled && displayActive && (integratedSource || sourceHasActiveOutput);
}
}
