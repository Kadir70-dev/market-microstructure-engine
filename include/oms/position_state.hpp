#pragma once
#include "exec/exec_types.hpp"
namespace oms {
[[nodiscard]] constexpr bool legal(exec::PositionState a,exec::PositionState b) noexcept {
 using S=exec::PositionState;
 if(a==S::opening)return b==S::open||b==S::reconcile_unknown;
 if(a==S::open)return b==S::modify_pending||b==S::closing||b==S::reconcile_unknown;
 if(a==S::modify_pending)return b==S::open||b==S::reconcile_unknown;
 if(a==S::closing)return b==S::closed||b==S::reconcile_unknown;
 if(a==S::reconcile_unknown)return b==S::open||b==S::closed;
 return false;
}
}
