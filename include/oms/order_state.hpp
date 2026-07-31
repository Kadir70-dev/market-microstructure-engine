#pragma once
#include "exec/exec_types.hpp"
namespace oms {
[[nodiscard]] constexpr bool legal(exec::OrderState a,exec::OrderState b) noexcept {
 using S=exec::OrderState;
 if(a==S::new_order)return b==S::risk_rejected||b==S::pending_send;
 if(a==S::pending_send)return b==S::sent;
 if(a==S::sent)return b==S::acknowledged||b==S::rejected||b==S::unknown;
 if(a==S::acknowledged)return b==S::partially_filled||b==S::filled||b==S::cancel_pending||b==S::expired;
 if(a==S::partially_filled)return b==S::partially_filled||b==S::filled||b==S::cancel_pending;
 if(a==S::cancel_pending)return b==S::cancelled||b==S::filled||b==S::unknown;
 if(a==S::unknown)return exec::is_terminal(b);
 return false;
}
}
