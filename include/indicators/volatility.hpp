#pragma once

#include <string>
#include <vector>

// Scale-invariant rolling volatility: stddev of log-returns across the window.
// Works identically for EUR/USD (~1.08), gold (~2300), crude/USO (~140) —
// the prior /100000 normalizer was calibrated only for EUR/USD scale.
// Empty / single-element windows return 0.
double calculateVolatility(
    const std::vector<double>& prices
);

// Buckets the score into LOW / MEDIUM / HIGH. Thresholds expressed as
// stddev of log-returns per ~30s tick. Replace with empirical percentiles
// from SQLite history once a few weeks of data have accumulated.
std::string classifyVolatility(
    double score
);
