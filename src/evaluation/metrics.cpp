#include "evaluation/metrics.hpp"

namespace evaluation {

namespace {

void step(
    HorizonStats& h,
    bool is_bull,
    bool is_bear,
    double raw,
    double cost
) {

    if(is_bull) {
        h.n_bull++;
        if(raw > 0.0)  h.bull_hits++;
        if(raw > cost) h.bull_hits_net++;     // beat the round-trip cost
        h.sum_dir_return += raw;
        h.sum_dir_cost   += cost;
    }
    else if(is_bear) {
        h.n_bear++;
        if(raw < 0.0)   h.bear_hits++;
        if(raw < -cost) h.bear_hits_net++;
        h.sum_dir_return += -raw;
        h.sum_dir_cost   += cost;
    }
}

double safe_div(double num, int den) {
    return den > 0 ? num / (double)den : 0.0;
}

double safe_div(int num, int den) {
    return den > 0 ? (double)num / (double)den : 0.0;
}

}

void accumulate(GroupStats& g, const Observation& obs) {

    g.n_total++;

    bool is_bull = (obs.momentum == "Bullish");
    bool is_bear = (obs.momentum == "Bearish");

    if(obs.has_60)  step(g.h60,  is_bull, is_bear, obs.ret_60,  obs.round_trip_cost);
    if(obs.has_300) step(g.h300, is_bull, is_bear, obs.ret_300, obs.round_trip_cost);
    if(obs.has_900) step(g.h900, is_bull, is_bear, obs.ret_900, obs.round_trip_cost);
}

GroupMetrics finalize(const GroupStats& g) {

    GroupMetrics m;

    // Gross.
    m.bull_acc_60  = safe_div(g.h60.bull_hits,  g.h60.n_bull);
    m.bull_acc_300 = safe_div(g.h300.bull_hits, g.h300.n_bull);
    m.bull_acc_900 = safe_div(g.h900.bull_hits, g.h900.n_bull);

    m.bear_acc_60  = safe_div(g.h60.bear_hits,  g.h60.n_bear);
    m.bear_acc_300 = safe_div(g.h300.bear_hits, g.h300.n_bear);
    m.bear_acc_900 = safe_div(g.h900.bear_hits, g.h900.n_bear);

    m.n_dir_60  = g.h60.n_bull  + g.h60.n_bear;
    m.n_dir_300 = g.h300.n_bull + g.h300.n_bear;
    m.n_dir_900 = g.h900.n_bull + g.h900.n_bear;

    m.mean_ret_60  = safe_div(g.h60.sum_dir_return,  m.n_dir_60);
    m.mean_ret_300 = safe_div(g.h300.sum_dir_return, m.n_dir_300);
    m.mean_ret_900 = safe_div(g.h900.sum_dir_return, m.n_dir_900);

    // Net.
    m.bull_acc_net_60  = safe_div(g.h60.bull_hits_net,  g.h60.n_bull);
    m.bull_acc_net_300 = safe_div(g.h300.bull_hits_net, g.h300.n_bull);
    m.bull_acc_net_900 = safe_div(g.h900.bull_hits_net, g.h900.n_bull);

    m.bear_acc_net_60  = safe_div(g.h60.bear_hits_net,  g.h60.n_bear);
    m.bear_acc_net_300 = safe_div(g.h300.bear_hits_net, g.h300.n_bear);
    m.bear_acc_net_900 = safe_div(g.h900.bear_hits_net, g.h900.n_bear);

    m.mean_cost_60  = safe_div(g.h60.sum_dir_cost,  m.n_dir_60);
    m.mean_cost_300 = safe_div(g.h300.sum_dir_cost, m.n_dir_300);
    m.mean_cost_900 = safe_div(g.h900.sum_dir_cost, m.n_dir_900);

    m.mean_net_ret_60  = m.mean_ret_60  - m.mean_cost_60;
    m.mean_net_ret_300 = m.mean_ret_300 - m.mean_cost_300;
    m.mean_net_ret_900 = m.mean_ret_900 - m.mean_cost_900;

    return m;
}

const char* confidenceBand(int conf) {

    if(conf < 30) return "0-29";
    if(conf < 60) return "30-59";
    return "60-100";
}

}
