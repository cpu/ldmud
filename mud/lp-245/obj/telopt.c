#pragma strict_types, warn_dead_code, warn_unused_values

#if !defined(DEBUG)
#define DEBUG(X) debug_message(sprintf("%s:%s %Q\n", __FILE__, __FUNCTION__, X))
#endif

public void telopt_negotiate(int action, int option, int *optdata) {
  DEBUG(sprintf("action = %d option = %d optdata = %Q", action, option, optdata);
}
