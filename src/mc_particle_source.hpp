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
// generator publishes the same products — the "mc_particles" collection
// (std::vector<SHiP::MCParticle>) and the "event_header" record
// (SHiP::EventHeader), both on the "event" layer — so the bundle
// construction is factored out here.
//
// The header carries the per-event weight. Sources that sample uniformly
// leave the generator empty and get the unweighted default (weight 1.0,
// no originating id), which keeps the output schema identical across all
// workflows; sources reading a weighted record (eventcalc_source) pass a
// generator.
//
// Implicit providers must name the stage their products belong to, and
// Phlex rejects the reserved "CURRENT". Phlex does not pass the job's stage
// to sources, so each generator source reads it from a `stage` key in its
// own configuration block; the workflows set it to the job's stage.

#pragma once

#include <SHiP/EventHeader.hpp>
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

// Generator signature: produce the metadata for a single data cell. May be
// empty, in which case the unweighted default is published.
using event_header_generator =
    std::function<SHiP::EventHeader(phlex::data_cell_index const&)>;

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
// "mc_particles" and "event_header" products in the given stage, honouring
// the framework's product selector. The generators are invoked once per data
// cell and their results type-erased into Phlex products.
inline phlex::provider_bundles mc_particle_provider_bundles(
    phlex::product_selector const& selector, std::string const& stage,
    mc_particle_generator generate, phlex::concurrency max_concurrency,
    event_header_generator generate_header = {}) {
  using namespace phlex::experimental;

  phlex::provider_bundles bundles;
  std::string const layer = "event";

  product_specification particles_spec{
      algorithm_name::create("mc_particles"), identifier{"particles"},
      make_type_id<std::vector<SHiP::MCParticle>>()};

  if (selector.match(particles_spec, identifier{layer}, identifier{stage})) {
    bundles.push_back(phlex::provider_bundle{
        .provider_function =
            [generate = std::move(generate)](phlex::data_cell_index const& id)
            -> product_ptr { return product_for(generate(id)); },
        .max_concurrency = max_concurrency,
        .spec = std::move(particles_spec),
        .layer = layer,
        .stage = stage});
  }

  product_specification header_spec{algorithm_name::create("event_header"),
                                    identifier{"header"},
                                    make_type_id<SHiP::EventHeader>()};

  if (selector.match(header_spec, identifier{layer}, identifier{stage})) {
    bundles.push_back(phlex::provider_bundle{
        .provider_function =
            [generate_header = std::move(generate_header)](
                phlex::data_cell_index const& id) -> product_ptr {
          // Unweighted default: every event counts once, no provenance.
          if (!generate_header) {
            return product_for(SHiP::EventHeader{});
          }
          return product_for(generate_header(id));
        },
        .max_concurrency = max_concurrency,
        .spec = std::move(header_spec),
        .layer = layer,
        .stage = stage});
  }

  return bundles;
}

}  // namespace aegir
