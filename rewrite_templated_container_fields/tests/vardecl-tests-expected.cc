// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <vector>

#include "base/memory/raw_ptr.h"

struct S {};

struct obj {
  // Expected rewrite: const std::vector<raw_ptr<S>>& get();
  const std::vector<raw_ptr<S>>& get() { return member; }

  // Expected rewrite: std::vector<raw_ptr<S>> member;
  std::vector<raw_ptr<S>> member;
};

// No rewrite expected.
std::vector<S*> get_value() {
  return {};
}

// No rewrite expected.
std::vector<S*>* get_ptr() {
  return nullptr;
}

// No rewrite expected.
std::vector<S*> unrelated_fct() {
  return {};
}

void fct() {
  obj o;

  {
    // Expected rewrite: std::vector<raw_ptr<S>> temp = o.member;
    std::vector<raw_ptr<S>> temp = o.member;
    // Expected rewrite: std::vector<raw_ptr<S>> temp2{temp};
    std::vector<raw_ptr<S>> temp2{temp};
  }

  {
    // Expected rewrite: std::vector<raw_ptr<S>>& temp = o.member;
    std::vector<raw_ptr<S>>& temp = o.member;
    // Expected rewrite: std::vector<raw_ptr<S>> temp2{temp};
    std::vector<raw_ptr<S>> temp2{temp};
  }

  {
    // Expected rewrite: const std::vector<raw_ptr<S>>& temp = o.member;
    const std::vector<raw_ptr<S>>& temp = o.member;
    // Expected rewrite: std::vector<raw_ptr<S>> temp2{temp};
    std::vector<raw_ptr<S>> temp2{temp};
  }

  {
    // Expected rewrite: std::vector<raw_ptr<S>>* temp = &o.member;
    std::vector<raw_ptr<S>>* temp = &o.member;
    // Expected rewrite: std::vector<raw_ptr<S>> temp2{*temp};
    std::vector<raw_ptr<S>> temp2{*temp};
  }

  {
    // Expected rewrite: []() -> std::vector<raw_ptr<S>> { return {}; };
    auto init = []() -> std::vector<raw_ptr<S>> { return {}; };
    o.member = init();
    // Expected rewrite: std::vector<raw_ptr<S>> temp = init();
    std::vector<raw_ptr<S>> temp = init();
  }

  {
    // Expected rewrite:
    // [&]() -> std::vector<raw_ptr<S>>* { return &o.member; };
    auto fct = [&]() -> std::vector<raw_ptr<S>>* { return &o.member; };
    // Expected rewrite: std::vector<raw_ptr<S>> a = *fct();
    std::vector<raw_ptr<S>> a = *fct();
    // Expected rewrite:
    // std::vector<raw_ptr<S>>::iterator it = (*fct()).begin();
    std::vector<raw_ptr<S>>::iterator it = fct()->begin();
  }

  {
    // Expected rewrite:
    //        [&]() -> const std::vector<raw_ptr<S>>& { return o.member; };
    auto fct = [&]() -> const std::vector<raw_ptr<S>>& { return o.member; };
    // Expected rewrite: std::vector<raw_ptr<S>> a = &fct();
    const std::vector<raw_ptr<S>>* a = &fct();
    // Expected rewrite:
    // std::vector<raw_ptr<S>>::const_iterator it = fct().begin();
    std::vector<raw_ptr<S>>::const_iterator it = fct().begin();
  }

  {
    // Expected rewrite: std::vector<raw_ptr<S>>::iterator
    std::vector<raw_ptr<S>>::iterator it = o.member.begin();
    // Expected rewrite: std::vector<raw_ptr<S>>::reverse_iterator
    std::vector<raw_ptr<S>>::reverse_iterator it2 = o.member.rbegin();
    // Expected rewrite: std::vector<raw_ptr<S>>::const_iterator
    std::vector<raw_ptr<S>>::const_iterator it3 = o.member.cbegin();
    // Expected rewrite: std::vector<raw_ptr<S>>::const_reverse_iterator
    std::vector<raw_ptr<S>>::const_reverse_iterator it4 = o.member.crbegin();
  }

  {
    // Expected rewrite: std::vector<raw_ptr<S>>::iterator
    std::vector<raw_ptr<S>>::iterator it = o.member.end();
    // Expected rewrite: std::vector<raw_ptr<S>>::reverse_iterator
    std::vector<raw_ptr<S>>::reverse_iterator it2 = o.member.rend();
    // Expected rewrite: std::vector<raw_ptr<S>>::const_iterator
    std::vector<raw_ptr<S>>::const_iterator it3 = o.member.cend();
    // Expected rewrite: std::vector<raw_ptr<S>>::const_reverse_iterator
    std::vector<raw_ptr<S>>::const_reverse_iterator it4 = o.member.crend();
  }

  {
    // create a link with a member to propagate the rewrite.
    std::vector<raw_ptr<S>> a = o.member;
    // Expected rewrite: std::vector<raw_ptr<S>> b;
    std::vector<raw_ptr<S>> b;
    // Expected rewrite: b = (a.size() > 0) ? a : std::vector<raw_ptr<S>>();
    b = (a.size() > 0) ? a : std::vector<raw_ptr<S>>();
  }
}
