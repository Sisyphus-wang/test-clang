// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

namespace blink {

class Task {
 public:
  // Already style-compliant methods shouldn't change.
  void OutputDebugString() {}

  // Tests that the declarations for methods are updated.
  void doTheWork();
  virtual void reallyDoTheWork() = 0;

  // Note: this is purposely copyable and assignable, to make sure the Clang
  // tool doesn't try to emit replacements for things that aren't explicitly
  // written.

  // Overloaded operators should not be rewritten.
  Task& operator++() {
    return *this;
  }

  // Conversion functions should not be rewritten.
  explicit operator int() const {
    return 42;
  }

  // These are special functions that we don't rename so that range-based
  // for loops and STL things work.
  void begin() {}
  void end() {}
  void rbegin() {}
  void rend() {}
  // The trace() method is used by Oilpan, we shouldn't rename it.
  void trace() {}
};

class Other {
  // Static begin/end/trace don't count, and should be renamed.
  static void begin() {}
  static void end() {}
  static void trace() {}
};

// Test that the actual method definition is also updated.
void Task::doTheWork() {
  reallyDoTheWork();
}

}  // namespace blink

namespace Moo {

// Test that overrides from outside the Blink namespace are also updated.
class BovineTask : public blink::Task {
 public:
  void reallyDoTheWork() override;
};

void BovineTask::reallyDoTheWork() {
  doTheWork();
  // Calls via an overridden method should also be updated.
  reallyDoTheWork();
}

// Finally, test that method pointers are also updated.
void F() {
  void (blink::Task::*p1)() = &blink::Task::doTheWork;
  void (blink::Task::*p2)() = &BovineTask::doTheWork;
  void (blink::Task::*p3)() = &blink::Task::reallyDoTheWork;
  void (BovineTask::*p4)() = &BovineTask::reallyDoTheWork;
}

}  // namespace Moo
