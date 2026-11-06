cartwheel_add_test(stack_test
  SOURCES stack_test.cc
  LIBS cartwheel::gateway cartwheel::cart cartwheel::client
  LABELS integration)
