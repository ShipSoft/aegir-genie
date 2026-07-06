// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// mc_particle_source.hpp — shared helper for event-generator sources
//
// Vendored unchanged from aegir (src/mc_particle_source.hpp) pending a shared
// helper package; keep in sync with the original. LGPL-3.0-or-later, see
// README.md § Licensing.
//
// Phlex 0.3.0 lets a source register its data-product providers implicitly:
// a phlex::source implements create_providers(selector), returning the
// provider bundles that satisfy the requested product. Every aegir event
// generator publishes the same product — the "mc_particles" collection
// (std::vector<SHiP::MCParticle>) on the "event" layer — so the bundle
// construction is factored out here.

#pragma once

#include <SHiP/MCParticle.hpp>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "phlex/concurrency.hpp"
#include "phlex/core/product_selector.hpp"
#include "phlex/model/data_cell_index.hpp"
#include "phlex/model/products.hpp"
#include "phlex/source.hpp"

namespace aegir {

// Generator signature: produce the particles for a single data cell (event).
using mc_particle_generator =
    std::function<std::vector<SHiP::MCParticle>(phlex::data_cell_index const&)>;

// Build the implicit-provider bundle(s) for a source emitting the
// "mc_particles" product, honouring the framework's product selector. The
// generator is invoked once per data cell and its result type-erased into a
// Phlex product.
inline phlex::detail::provider_bundles mc_particle_provider_bundles(
    phlex::product_selector const& selector, mc_particle_generator generate,
    phlex::concurrency max_concurrency) {
  using namespace phlex::experimental;
  using namespace phlex::detail;

  provider_bundles bundles;
  product_specification spec{algorithm_name::create("mc_particles"),
                             identifier{"particles"},
                             make_type_id<std::vector<SHiP::MCParticle>>()};
  std::string const layer = "event";
  std::string const stage = "CURRENT";

  if (selector.match(spec, identifier{layer}, identifier{stage})) {
    bundles.push_back(provider_bundle{
        .provider_function =
            [generate = std::move(generate)](phlex::data_cell_index const& id)
            -> product_ptr { return product_for(generate(id)); },
        .max_concurrency = max_concurrency,
        .spec = std::move(spec),
        .layer = layer,
        .stage = stage});
  }
  return bundles;
}

}  // namespace aegir
