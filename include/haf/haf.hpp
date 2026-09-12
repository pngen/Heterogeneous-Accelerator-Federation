// Heterogeneous Accelerator Federation - umbrella header.
//
// Includes the complete public, vendor-neutral surface of the runtime. A
// downstream consumer that wants the whole API can include this single header;
// consumers that care about compile time should include the specific headers
// they use, as the rest of the repository does.

#ifndef HAF_HAF_HPP
#define HAF_HAF_HPP

// Core primitives
#include "haf/core/bytes.hpp"
#include "haf/core/errors.hpp"
#include "haf/core/hash.hpp"
#include "haf/core/idgen.hpp"
#include "haf/core/ids.hpp"
#include "haf/core/limits.hpp"
#include "haf/core/status.hpp"
#include "haf/core/time.hpp"
#include "haf/core/version.hpp"

// Model
#include "haf/model/accelerator.hpp"
#include "haf/model/capability.hpp"
#include "haf/model/capability_key.hpp"
#include "haf/model/compatibility.hpp"
#include "haf/model/evidence.hpp"
#include "haf/model/membership.hpp"
#include "haf/model/migration.hpp"
#include "haf/model/policy.hpp"
#include "haf/model/portability.hpp"
#include "haf/model/taxonomy.hpp"
#include "haf/model/workload.hpp"

// Engines
#include "haf/engine/audit.hpp"
#include "haf/engine/compatibility_engine.hpp"
#include "haf/engine/migration_planner.hpp"
#include "haf/engine/portability_engine.hpp"
#include "haf/engine/ranking.hpp"

// Runtime
#include "haf/federation/authority.hpp"
#include "haf/federation/events.hpp"
#include "haf/federation/federation.hpp"
#include "haf/federation/snapshot.hpp"

// Persistence
#include "haf/persist/file_store.hpp"
#include "haf/persist/store.hpp"

// Adapters (vendor-neutral surface: the CUDA adapter is a separate header and
// is only available when the build produced it)
#include "haf/adapters/adapter.hpp"
#include "haf/adapters/registry.hpp"
#include "haf/adapters/rocm_synthetic.hpp"
#include "haf/adapters/synthetic.hpp"

#endif  // HAF_HAF_HPP
