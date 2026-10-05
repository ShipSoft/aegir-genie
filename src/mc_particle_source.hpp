// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// mc_particle_source.hpp — shared helper for event-generator sources
//
// Vendored unchanged from aegir (src/mc_particle_source.hpp) pending a shared
// helper package; keep in sync with the original. LGPL-3.0-or-later, see
// README.md § Licensing.
//
// A Phlex source registers its data-product providers implicitly: a
// phlex::source implements create_providers(selector), returning the
// provider bundles that satisfy the requested product. Every aegir event
// generator publishes the same product — the "mc_particles" collection
// (std::vector<SHiP::MCParticle>) on the "event" layer — so the bundle
// construction is factored out here.
//
// Implicit providers must name the stage their products belong to, and
// Phlex rejects the reserved "CURRENT". Phlex does not pass the job's stage
// to sources, so each generator source reads it from a `stage` key in its
// own configuration block; the workflows set it to the job's stage.

#pragma once

#include <SHiP/MCParticle.hpp>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "phlex/concurrency.hpp"
#include "phlex/configuration.hpp"
#include "phlex/core/product_selector.hpp"
#include "phlex/model/data_cell_index.hpp"
#include "phlex/model/products.hpp"
#include "phlex/source.hpp"

namespace aegir {

// Generator signature: produce the particles for a single data cell (event).
using mc_particle_generator =
    std::function<std::vector<SHiP::MCParticle>(phlex::data_cell_index const&)>;

// Read the required `stage` key of a generator source's configuration block.
[[nodiscard]] inline std::string source_stage(
    phlex::configuration const& config, std::string const& component) {
  auto stage = config.get_if_present<std::string>("stage");
  if (!stage || stage->empty() || *stage == "CURRENT") {
    throw std::runtime_error(
        component +
        ": the source block needs a 'stage' key naming the job's stage "
        "(e.g. stage: 'simulation'); 'CURRENT' is reserved");
  }
  return std::move(*stage);
}

// Build the implicit-provider bundle(s) for a source emitting the
// "mc_particles" product in the given stage, honouring the framework's
// product selector. The generator is invoked once per data cell and its
// result type-erased into a Phlex product.
inline phlex::provider_bundles mc_particle_provider_bundles(
    phlex::product_selector const& selector, std::string const& stage,
    mc_particle_generator generate, phlex::concurrency max_concurrency) {
  using namespace phlex::experimental;

  phlex::provider_bundles bundles;
  product_specification spec{algorithm_name::create("mc_particles"),
                             identifier{"particles"},
                             make_type_id<std::vector<SHiP::MCParticle>>()};
  std::string const layer = "event";

  if (selector.match(spec, identifier{layer}, identifier{stage})) {
    bundles.push_back(phlex::provider_bundle{
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
