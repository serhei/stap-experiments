// -*- C++ -*-
// Copyright (C) 2012-2013 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.
//
// ---
//
// This file incorporates code from the re2c project; please see
// the file README.stapregex for details.

#include <string>
#include <iostream>
#include <sstream>
#include <set>
#include <list>
#include <map>
#include <vector>
#include <stack>
#include <queue>
#include <utility>

#include "translator-output.h"

#include "stapregex-parse.h"
#include "stapregex-tree.h"
#include "stapregex-dfa.h"

// Uncomment to show result of ins (NFA) compilation:
//#define STAPREGEX_DEBUG_INS
// Uncomment to display result of DFA compilation in a compact format:
//#define STAPREGEX_DEBUG_DFA
// Uncomment to have the generated engine do a trace of visited states:
//#define STAPREGEX_DEBUG_MATCH

// Uncomment for a detailed walkthrough of the tagged-NFA conversion:
//#define STAPREGEX_DEBUG_TNFA

using namespace std;

namespace stapregex {

regexp *pad_re = NULL;
regexp *fail_re = NULL;

dfa *
stapregex_compile (regexp *re, const std::string& match_snippet,
                   const std::string& fail_snippet)
{
  if (pad_re == NULL) {
    // build regexp for ".*"
    pad_re = make_dot ();
    pad_re = new close_op (pad_re, true); // -- prefer shorter match
    pad_re = new alt_op (pad_re, new null_op, true); // -- prefer second match
  }
  if (fail_re == NULL) {
    // build regexp for ".*$", but allow '\0' and support fail outcome
    fail_re = make_dot (true); // -- allow '\0'
    fail_re = new close_op (fail_re, true); // -- prefer shorter match
    fail_re = new alt_op (fail_re, new null_op, true); // -- prefer second match
    fail_re = new cat_op (fail_re, new anchor_op('$'));
    fail_re = new rule_op(fail_re, 0);
    // XXX: this approach creates one extra spurious-but-safe state
    // (safe because the matching procedure stops after encountering '\0')
  }

  vector<string> outcomes(2);
  outcomes[0] = fail_snippet;
  outcomes[1] = match_snippet;

  int num_tags = re->num_tags;

  // Pad & wrap re in appropriate rule_ops to control match behaviour:
  bool anchored = re->anchored ();
  if (!anchored) re = new cat_op(pad_re, re); // -- left-padding
  re = new rule_op(re, 1);
  re = new alt_op(re, fail_re);

#ifdef STAPREGEX_DEBUG_INS
  cerr << "RESULTING INS FROM REGEX " << re << ":" << endl;
#endif

  ins *i = re->compile();

#ifdef STAPREGEX_DEBUG_INS
  for (const ins *j = i; (j - i) < re->ins_size() + 1; )
    {
      j = show_ins(cerr, j, i); cerr << endl;
    }
  cerr << endl;
#endif
  
  // TODOXXX optimize ins as in re2c

  dfa *d = new dfa(i, num_tags, outcomes);

  // Carefully deallocate temporary scaffolding:
  if (!anchored) delete ((rule_op*) ((alt_op*) re)->a)->re; // -- new cat_op
  delete ((alt_op*) re)->a; // -- new rule_op
  delete re; // -- new alt_op
  // NB: deleting a regular expression DOES NOT deallocate its
  // children. The original re parameter is presumed to be retained
  // indefinitely as part of a stapdfa table, or such....

  return d;
}

// ------------------------------------------------------------------------

/* Now follows the heart of the tagged-DFA algorithm. This is a basic
   implementation of the algorithm described in Ville Laurikari's
   Masters thesis and summarized in the paper "NFAs with Tagged
   Transitions, their Conversion to Deterministic Automata and
   Application to Regular Expressions"
   (http://laurikari.net/ville/spire2000-tnfa.pdf).

   TODOXXX: The following does not contain a fully working
   implementation of the tagging support, but only of the regex
   matching.

   HERE BE DRAGONS (and not the friendly kind) */

/* Functions to deal with relative transition priorities: */

arc_priority
refine_higher(const arc_priority& a)
{
  return make_pair(2 * a.first + 1, a.second + 1);
}

arc_priority
refine_lower (const arc_priority& a)
{
  return make_pair(2 * a.first, a.second + 1);
}

int
arc_compare (const arc_priority& a, const arc_priority& b)
{
  unsigned long x = a.first;
  unsigned long y = b.first;

  if (a.second > b.second)
    x = x << (a.second - b.second);
  else if (a.second < b.second)
    y = y << (b.second - a.second);

  // Special case: 0/n </> 0/m iff m </> n.
  // This is because such priorities are obtained by refine_lower().
  if (x == 0 && y == 0)
    return ( a.second == b.second ? 0 : a.second < b.second ? 1 : -1 );

  return ( x == y ? 0 : x < y ? -1 : 1 );
}

/* Manage the linked list of states in a DFA: */

state::state (dfa *owner, state_kernel *kernel)
  : owner(owner), label(~0), next(NULL), kernel(kernel),
    accepts(false), accept_outcome(0) {}

state *
dfa::add_state (state *s)
{
  s->label = nstates++;
  
  if (last == NULL)
    {
      last = s;
      first = last;
    }
  else
    {
      // append to the end
      last->next = s;
      last = last->next;
    }

  return last;
}

/* Operations to build a simple kernel prior to taking closure: */

/* Create a new kernel_point in kernel with empty map items. */
void
add_kernel (state_kernel *kernel, ins *i)
{
  kernel_point point;
  point.i = i;
  point.priority = make_pair(0,0);
  // NB: point->map_items is empty

  kernel->push_back(point);
}

state_kernel *
make_kernel (ins *i)
{
  state_kernel *kernel = new state_kernel;
  add_kernel (kernel, i);
  return kernel;
}

/* Compute the set of kernel_points that are 'tag-wise unambiguously
   reachable' from a given initial set of points. Absent tagging, this
   becomes a bog-standard NFA e_closure construction. */
state_kernel *
te_closure (state_kernel *start, int ntags, bool is_initial = false)
{
  state_kernel *closure = new state_kernel(*start);
  stack<kernel_point> worklist;
  // XXX: state_kernel is a list<kernel_point> so we avoid iterator
  // invalidation and make a new copy of each kernel_point from start

  /* To avoid searching through closure incessantly when retrieving
     information about existing elements, the following caches are
     needed: */
  vector<unsigned> max_tags (ntags, 0);
  map<ins *, list<list<kernel_point>::iterator> > closure_map;

  /* Reset priorities and cache initial elements of closure: */
  for (state_kernel::iterator it = closure->begin();
       it != closure->end(); it++)
    {
      it->priority = make_pair(0,0);
      worklist.push(*it);

      // Store the element in relevant caches:

      for (list<map_item>::const_iterator jt = it->map_items.begin();
           jt != it->map_items.end(); jt++)
        max_tags[jt->first] = max(jt->second, max_tags[jt->first]);

      closure_map[it->i].push_back(it);
    }

  while (!worklist.empty())
    {
      kernel_point point = worklist.top(); worklist.pop();

      // Identify e-transitions depending on the opcode.
      // There are at most two e-transitions emerging from an insn.
      // If we have two e-transitions, the 'other' has higher priority.

      ins *target = NULL; int tag = -1;
      ins *other_target = NULL; int other_tag = -1;

      bool do_split = false;

      if (point.i->i.tag == TAG)
        {
          target = &point.i[1];
          tag = (int) point.i->i.param;
        }
      else if (point.i->i.tag == FORK && point.i == (ins *) point.i->i.link)
        {
          /* Workaround for a FORK that points to itself: */
          target = &point.i[1];
        }
      else if (point.i->i.tag == FORK)
        {
          do_split = true;
          // Relative priority of two e-transitions depends on param:
          if (point.i->i.param)
            {
              // Prefer jumping to link.
              target = &point.i[1];
              other_target = (ins *) point.i->i.link;
            }
          else
            {
              // Prefer stepping to next instruction.
              target = (ins *) point.i->i.link;
              other_target = &point.i[1];
            }
        }
      else if (point.i->i.tag == GOTO)
        {
          target = (ins *) point.i->i.link;
        }
      else if (point.i->i.tag == INIT && is_initial)
        {
          target = &point.i[1];
        }

      bool already_found;

      // Data for the endpoint of the first transition:
      kernel_point next;
      next.i = target;
      next.priority = do_split ? refine_lower(point.priority) : point.priority;
      next.map_items = point.map_items;

      // Date for the endpoint of the second transition:
      kernel_point other_next;
      other_next.i = other_target;
      other_next.priority = do_split ? refine_higher(point.priority) : point.priority;
      other_next.map_items = point.map_items;

      // Do infinite-loop-check:
      other_next.parents = point.parents;
      if (point.parents.find(other_next.i) != point.parents.end())
        {
          other_target = NULL;
          other_tag = -1;
        }
      other_next.parents.insert(other_next.i);

      next.parents = point.parents;
      if (point.parents.find(next.i) != point.parents.end())
        {
          // target = NULL;
          // tag = -1;
          // <- XXX will be overwritten by other_target / other_tag immediately
          goto next_target;
        }
      next.parents.insert(next.i);

    another_transition:
      if (target == NULL)
        continue;

      // Deal with the current e-transition:

      if (tag >= 0)
        {
          /* Delete all existing next.map_items of the form m[tag,x]. */
          for (list<map_item>::iterator it = next.map_items.begin();
               it != next.map_items.end(); )
            if (it->first == (unsigned) tag)
              {
                list<map_item>::iterator next_it = it;
                next_it++;
                next.map_items.erase (it);
                it = next_it;
              }
            else it++;

          /* Add m[tag,x] to next.map_items, where x is the smallest
             nonnegative integer such that m[tag,x] does not occur
             anywhere in closure. Then update the cache. */
          unsigned x = max_tags[tag];
          next.map_items.push_back(make_pair(tag, ++x));
          max_tags[tag] = x;
        }

      /* Deal with similar transitions that have a different priority: */
      already_found = false;
      for (list<list<kernel_point>::iterator>::iterator it
             = closure_map[next.i].begin();
           it != closure_map[next.i].end(); )
        {
          // NB: it is an iterator into closure_map[next.i],
          // while *it is an iterator into closure

          int result = arc_compare((*it)->priority, next.priority);
          if (result == 0)
            cerr << "**DEBUG** identical arc_priorities "
                 << (*it)->priority << " " << next.priority << endl;
          assert (result != 0); // TODOXXX expected to fail? however, the proper semantics for this case are not yet clear to me

          if (result > 0) {
            // next.priority is higher, delete existing element
            closure->erase(*it);

            // obnoxious shuffle to avoid iterator invalidation
            list<list<kernel_point>::iterator>::iterator old_it = it;
            it++;
            closure_map[next.i].erase(old_it);
            continue;
          } else { // result < 0
            // next.priority is lower, skip adding next
            already_found = true;
          }

          it++;
        }

      if (!already_found) {
        // Store the element in closure:
        closure->push_back(next);
        worklist.push(next);

        // Store the element in relevant caches:

        list<kernel_point>::iterator next_it = closure->end();
        next_it --; // XXX rewind to just-pushed element
        closure_map[next.i].push_back(next_it);

        for (list<map_item>::iterator jt = next.map_items.begin();
             jt != next.map_items.end(); jt++)
          max_tags[jt->first] = max(jt->second, max_tags[jt->first]);

      }

    next_target:
      // Now move to dealing with the second e-transition, if any.
      target = other_target; other_target = NULL;
      tag = other_tag; other_tag = -1;
      next = other_next;

      goto another_transition;
    }

  return closure;
}

/* Helpers for constructing span table: */

bool
same_ins(list<kernel_point> &e1, list<kernel_point> &e2)
{
  set<ins *> s1;
  for (list<kernel_point>::iterator it = e1.begin();
       it != e1.end(); it++)
    s1.insert(it->i);
  set<ins *> s2;
  for (list<kernel_point>::iterator it = e2.begin();
       it != e2.end(); it++)
    s2.insert(it->i);
  return s1 == s2;
}

/* Helpers for constructing TDFA actions: */

/* Find the set of reordering commands (if any) that will get us from
   state s to some existing state in the dfa (returns the state in
   question, appends reordering commands to r). Returns NULL is no
   suitable state is found. */
state *
dfa::find_equivalent (state *s, tdfa_action &action)
{
  state *answer = NULL;

  for (state_kernel::iterator it = s->kernel->begin();
       it != s->kernel->end(); it++)
    mark(it->i);

  /* Check kernels of existing states for size equivalence and for
     unmarked items (similar to re2c's original algorithm): */
  unsigned n = s->kernel->size();
  map<map_item, map_item> shift_map;
  map<map_item, map_item> shift_back;
  for (state *t = first; t != NULL; t = t->next)
    {
      if (t->kernel->size() == n)
        {
          for (state_kernel::iterator it = t->kernel->begin();
               it != t->kernel->end(); it++)
              if (!marked(it->i)) 
                goto next_state;

          // Check for existence of a reordering tdfa_action r that will
          // produce identical kernel_points with identical map values.

          // XXX In the below code, we search for more-or-less an
          // arbitrary permutation of map values.
          //
          // To simplify the algorithm, we could instead only check
          // where lower-index map values are missing from s and
          // replace them with higher-index map values. The paper
          // claims this leads to only a slight penalty in number of
          // TDFA states.

          // Mapping must be one-to-one; check consistency in both directions:
          shift_map.clear(); // map item of s -> map item of t
          shift_back.clear(); // map item of t -> map item of s

          for (state_kernel::iterator it = s->kernel->begin();
               it != s->kernel->end(); it++)
            {
              kernel_point *kp1 = &*it;
              kernel_point *kp2;

              // Find matching kernel_point in t:
              bool found_kp = false;
              for (state_kernel::iterator jt = t->kernel->begin();
                   jt != t->kernel->end(); jt++)
                if (kp1->i == jt->i)
                  {
                    // XXX check that ins appears only once
                    assert (!found_kp);
                    kp2 = &*jt; // TODO found matching point
                    found_kp = true;
                  }
              assert(found_kp);

              set<int> seen_tags;
              for (list<map_item>::iterator jt = kp1->map_items.begin();
                   jt != kp1->map_items.end(); jt++)
                {
                  map_item mt1 = *jt;
                  map_item mt2;

                  // XXX check that tag appears only once
                  assert (seen_tags.count(mt1.first) == 0);
                  seen_tags.insert(mt1.first);

                  // Find matching map_item in kp2
                  bool found_tag = false;
                  for (list<map_item>::iterator kt = kp2->map_items.begin();
                       kt != kp2->map_items.end(); kt++)
                    if (mt1.first == kt->first)
                      {
                        // XXX check that tag appears only once
                        assert (!found_tag);
                        mt2 = *kt;
                        found_tag = true;
                      }

                  if (!found_tag) // if no matching tag, can't use this state
                    goto next_state;
                  if (shift_map.count(mt1) != 0
                      && shift_map[mt1] != mt2) // if contradiction
                    goto next_state;
                  if (shift_back.count(mt2) != 0
                      && shift_back[mt2] != mt1) // if contradiction
                    goto next_state;

                  shift_map[mt1] = mt2;
                  shift_back[mt2] = mt1;
                }

              // XXX check that every tag in kp2 appears in seen_tag
              for (list<map_item>::iterator jt = kp2->map_items.begin();
                   jt != kp2->map_items.end(); jt++)
                {
                  int t2 = jt->first;
                  if (seen_tags.count(t2) == 0)
                    goto next_state;
                }
            }

#ifdef STAPREGEX_DEBUG_TNFA
          cerr << " -*- obtained valid reorder ";
          for (map<map_item, map_item>::iterator it = shift_map.begin();
               it != shift_map.end(); it++)
            if (it->first != it->second)
              cerr << it->first << "=>" << it->second << " ";
          cerr << "to existing state " << t->label << endl;
#endif

          tdfa_action r;
          // TODOXXX generate reordering commands based on the contents of shift_map[]

          answer = t;
          action.insert(action.end(), r.begin(), r.end()); // XXX append
          goto cleanup;
        }
    next_state:
      ;
    }

 cleanup:
  for (state_kernel::iterator it = s->kernel->begin();
       it != s->kernel->end(); it++)
    unmark(it->i);

  return answer;
}

/* Generate position-save commands for any map items in new_k that do
   not appear in old_k (old_k can be NULL). */
tdfa_action
dfa::compute_action (state_kernel *old_k, state_kernel *new_k)
{
  tdfa_action c;

  set<map_item> old_items;
  if (old_k != NULL)
    for (state_kernel::const_iterator it = old_k->begin();
         it != old_k->end(); it++)
      for (list<map_item>::const_iterator jt = it->map_items.begin();
           jt != it->map_items.end(); jt++)
        old_items.insert(*jt);

  // XXX: use a set, since we only need one position-save per new map item
  set<map_item> store_items;
  for (state_kernel::const_iterator it = new_k->begin();
       it != new_k->end(); it++)
    for (list<map_item>::const_iterator jt = it->map_items.begin();
         jt != it->map_items.end(); jt++)
      if (old_items.find(*jt) == old_items.end())
        store_items.insert(*jt);

  for (set<map_item>::iterator it = store_items.begin();
       it != store_items.end(); it++)
    {
      // append m[i,n] <- <curr position> to c
      tdfa_insn insn;
      insn.to = *it;
      insn.save_tag = false;
      insn.save_pos = true;
      c.push_back(insn);
    }

  return c;
}

tdfa_action
dfa::compute_finalizer (state *s)
{
  // TODO VERIFY THAT THIS WORKS -- CAN THERE BE CONFLICTS?
  tdfa_action c;
  assert (s->accept_kp != NULL);

  // iterate map items m[i,j]
  for (list<map_item>::iterator it = s->accept_kp->map_items.begin();
       it != s->accept_kp->map_items.end(); it++)
    {
      // append t[i] <- m[i,j] to c
      tdfa_insn insn;
      insn.from = *it;
      insn.save_tag = true;
      insn.save_pos = false;
      c.push_back(insn);
    }

  return c;
}

/* The main DFA-construction algorithm: */

dfa::dfa (ins *i, int ntags, vector<string>& outcome_snippets)
  : orig_nfa(i), nstates(0), ntags(ntags), outcome_snippets(outcome_snippets)
{
#ifdef STAPREGEX_DEBUG_TNFA
  cerr << "DFA CONSTRUCTION (ntags=" << ntags << "):" << endl;
#endif

  /* Initialize empty linked list of states: */
  first = last = NULL;

  ins *start = &i[0];
  state_kernel *seed_kernel = make_kernel(start);
  state_kernel *initial_kernel = te_closure(seed_kernel, ntags, true);
  delete seed_kernel;
  state *initial = add_state(new state(this, initial_kernel));
  queue<state *> worklist; worklist.push(initial);

  initializer = compute_action(NULL, initial_kernel);
#ifdef STAPREGEX_DEBUG_TNFA
  cerr << " - constructed initializer " << initializer << endl << endl;
#endif

  while (!worklist.empty())
    {
      state *curr = worklist.front(); worklist.pop();

      // Kernel points before and after each edge:
      vector<list<kernel_point> > edge_begin(NUM_REAL_CHARS);
      vector<list<kernel_point> > edge_end(NUM_REAL_CHARS);

      /* Using the CHAR instructions in kernel, build the initial
         table of spans for curr. Also check for final states. */

      for (list<kernel_point>::iterator it = curr->kernel->begin();
           it != curr->kernel->end(); it++)
        {
          if (it->i->i.tag == CHAR)
            {
              // Add a new kernel_point for each targeted insn:
              for (ins *j = &it->i[1]; j < (ins *) it->i->i.link; j++)
                {
                  // XXX: deallocate together with span table
                  kernel_point point;
                  point.i = (ins *) it->i->i.link;
                  point.priority = make_pair(0,0);
                  point.map_items = it->map_items; // copy map items

                  edge_begin[j->c.value].push_back(*it);
                  edge_end[j->c.value].push_back(point);
                }
            }
          else if (it->i->i.tag == ACCEPT)
            {
              /* In case of multiple accepting NFA states,
                 prefer the highest numbered outcome: */
              if (!curr->accepts || it->i->i.param > curr->accept_outcome)
                {
                  curr->accept_kp = &*it;
                  curr->accept_outcome = it->i->i.param;
                }
              curr->accepts = true;
            }
        }

      /* If the state was marked as accepting, add a finalizer: */
      if (curr->accepts)
        {
          assert(curr->finalizer.empty()); // XXX: only process a state once
          curr->finalizer = compute_finalizer(curr);
        }

      for (unsigned c = 0; c < NUM_REAL_CHARS; )
        {
          list<kernel_point> eb = edge_begin[c];
          list<kernel_point> ee = edge_end[c];
          assert (!ee.empty()); // XXX: ensured by fail_re in stapregex_compile

          span s;

          s.lb = c;

          // TODOXXX: what if list<ins *> is the same but map_items are not?
          while (++c < NUM_REAL_CHARS && same_ins(edge_end[c], ee)) ;

          s.ub = c - 1;

          s.reach_pairs = new state_kernel; // TODOXXX dealloc?
          s.jump_pairs = new state_kernel; // TODOXXX dealloc?

          for (list<kernel_point>::iterator it = eb.begin();
               it != eb.end(); it++)
            s.jump_pairs->push_back(*it);
          for (list<kernel_point>::iterator it = ee.begin();
               it != ee.end(); it++)
            s.reach_pairs->push_back(*it);

          curr->spans.push_back(s);
        }

      /* For each of the spans in curr, determine the reachable
         points assuming a character in the span. */
#ifdef STAPREGEX_DEBUG_TNFA
      cerr << "building transitions for state " << curr->label << ":" << endl;
#endif
      for (list<span>::iterator it = curr->spans.begin();
           it != curr->spans.end(); it++)
        {
          /* Set up candidate target state: */
          state_kernel *u_pairs = te_closure(it->reach_pairs, ntags);
          state *target = new state(this, u_pairs);

          /* Generate position-save commands for any map items
             that do not appear in TODOXXX the precursor to the edge: */
          // TODOXXX tdfa_action c = compute_action(curr->kernel, u_pairs);
          tdfa_action c = compute_action(it->jump_pairs, u_pairs);

          /* If there is a state t_prime in states such that some
             sequence of reordering commands r produces t_prime
             from target, use t_prime as the target state,
             appending the reordering commands to c. */
          state *t_prime = find_equivalent(target, c);
          if (t_prime != NULL)
            {
              assert (t_prime != target);
              delete target;
            }
          else
            {
              /* We need to actually add target to the dfa: */
              t_prime = target;
              add_state(t_prime);
              worklist.push(t_prime);
#ifdef STAPREGEX_DEBUG_TNFA
              cerr << " -*- add new state " << t_prime->label << endl;
#endif
            }

          /* Set the transition: */
          it->to = t_prime;
          it->action = c;
        }
#ifdef STAPREGEX_DEBUG_TNFA
      cerr << " -> constructed " << curr << endl;
#endif
    }
#ifdef STAPREGEX_DEBUG_TNFA
      cerr << endl;
#endif
}

dfa::~dfa ()
{
  state * s;
  while ((s = first))
    {
      first = s->next;
      delete s;
    }

  delete orig_nfa;
}

// ------------------------------------------------------------------------

// TODOXXX add emission instructions for tag_ops

void
span::emit_jump (translator_output *o, const dfa *d) const
{
#ifdef STAPREGEX_DEBUG_MATCH
  o->newline () << "printf(\" --> GOTO yystate%d\\n\", " << to->label << ");";
#endif

  // TODOXXX tags feature allows proper longest-match priority
  if (to->accepts)
    {
      emit_final(o, d);
    }
  else
    {
      o->newline () << "YYCURSOR++;";
      o->newline () << "goto yystate" << to->label << ";";
    }
}

/* Assuming the target DFA of the span is a final state, emit code to
   (TODOXXX) cleanup tags and exit with a final answer. */
void
span::emit_final (translator_output *o, const dfa *d) const
{
  assert (to->accepts); // XXX: must guarantee correct usage of emit_final()
  o->newline() << d->outcome_snippets[to->accept_outcome];
  o->newline() << "goto yyfinish;";
}

string c_char(char c)
{
  stringstream o;
  o << "'";
  print_escaped(o, c);
  o << "'";
  return o.str();
}

void
state::emit (translator_output *o, const dfa *d) const
{
  o->newline() << "yystate" << label << ": ";
#ifdef STAPREGEX_DEBUG_MATCH
  o->newline () << "printf(\"READ '%s' %c\", cur, *YYCURSOR);";
#endif
  o->newline() << "switch (*YYCURSOR) {";
  o->indent(1);
  for (list<span>::const_iterator it = spans.begin();
       it != spans.end(); it++)
    {
      // If we see a '\0', go immediately into an accept state:
      if (it->lb == '\0')
        {
          o->newline() << "case " << c_char('\0') << ":";
          it->emit_final(o, d); // TODOXXX extra function may be unneeded
        }

      // Emit labels to handle all the other elements of the span:
      for (unsigned c = max('\1', it->lb); c <= (unsigned) it->ub; c++) {
        o->newline() << "case " << c_char((char) c) << ":";
      }
      it->emit_jump(o, d);

      // TODOXXX handle a 'default' set of characters for the largest span...
      // TODOXXX optimize by accepting before end of string whenever possible... (also necessary for proper first-matched-substring selection)
    }
  o->newline(-1) << "}";
}

void
dfa::emit (translator_output *o) const
{
#ifdef STAPREGEX_DEBUG_DFA
  print(o);
#else
  o->newline() << "{";
  o->newline(1);

  // XXX: workaround for empty regex
  if (first->accepts)
    {
      o->newline() << outcome_snippets[first->accept_outcome];
      o->newline() << "goto yyfinish;";      
    }

  for (state *s = first; s; s = s->next)
    s->emit(o, this);

  o->newline() << "yyfinish: ;";
  o->newline(-1) << "}";
#endif
}

void
dfa::emit_tagsave (translator_output *, std::string,
                   std::string, std::string) const
{
  // TODOXXX implement after testing the preceding algorithms
}

// ------------------------------------------------------------------------

std::ostream&
operator << (std::ostream &o, const map_item& m)
{
  o << "m[" << m.first << "," << m.second << "]";
  return o;
}

std::ostream&
operator << (std::ostream &o, const tdfa_action& a)
{
  for (list<tdfa_insn>::const_iterator it = a.begin();
       it != a.end(); it++)
    {
      if (it != a.begin()) o << "; ";

      if (it->save_tag)
        o << "t[" << it->from.first << "] <- ";
      else
        o << it->to << " <- ";

      if (it->save_pos)
        o << "p";
      else
        o << it->from;
    }

  return o;
}

std::ostream&
operator << (std::ostream &o, const arc_priority& p)
{
  o << p.first << "/" << (1 << p.second);
  return o;
}

void
state::print (translator_output *o) const
{
  o->line() << "state " << label;

#ifdef STAPREGEX_DEBUG_TNFA
  // For debugging, also show the kernel:
  ins *base = owner->orig_nfa;
  o->line() << " w/kernel {";
  for (state_kernel::iterator it = kernel->begin();
       it != kernel->end(); it++)
    {
      if (it != kernel->begin()) o->line() << "; ";
      o->line() << (it->i - base);
      o->line() << "[" << it->priority << "]";
      if (!it->map_items.empty())
        {
          o->line() << ":";
          for (list<map_item>::iterator jt = it->map_items.begin();
               jt != it->map_items.end(); jt++)
            {
              if (jt != it->map_items.begin()) o->line() << ",";
              o->line() << *jt;
            }
        }
    }
  o->line() << "}";

  // Also print information for constructing reorderings:
  set<map_item> all_items;
  for (state_kernel::iterator it = kernel->begin();
       it != kernel->end(); it++)
    for (list<map_item>::iterator jt = it->map_items.begin();
         jt != it->map_items.end(); jt++)
      all_items.insert(*jt);
  if (!all_items.empty())
    {
      o->newline() << "  ";
      o->line() << " with map_items ";
      for (set<map_item>::iterator it = all_items.begin();
           it != all_items.end(); it++)
        o->line() << *it << " ";
    }

  if (accepts || !finalizer.empty())
    o->newline() << "  ";
#endif

  if (accepts)
    o->line() << " accepts " << accept_outcome;
  if (!finalizer.empty())
    o->line() << " with finalizer {" << finalizer << "}";

  // TODOXXX: factor this out to span::print()
  o->indent(1);
  for (list<span>::const_iterator it = spans.begin();
       it != spans.end(); it++)
    {
      o->newline() << "'";
      if (it->lb == it->ub)
        {
          print_escaped (o->line(), it->lb);
          o->line() << "  ";
        }
      else
        {
          print_escaped (o->line(), it->lb);
          o->line() << "-";
          print_escaped (o->line(), it->ub);
        }

      if (it->to != NULL)
        o->line() << "' -> " << it->to->label;
      else
        o->line() << "' -> <none>";

      if (!it->action.empty())
        o->line() << " {" << it->action << "}";
    }
  o->newline(-1);
}

void
state::print (std::ostream &o) const
{
  translator_output to(o); print(&to);
}

std::ostream&
operator << (std::ostream &o, const state *s)
{
  s->print(o);
  return o;
}

void
dfa::print (translator_output *o) const
{
  o->newline();
  for (state *s = first; s; s = s->next)
    {
      s->print(o);
      o->newline();
    }
  o->newline();
}

void
dfa::print (std::ostream& o) const
{
  translator_output to(o); print(&to);
}

std::ostream&
operator << (std::ostream& o, const dfa& d)
{
  d.print(o);
  return o;
}

std::ostream&
operator << (std::ostream &o, const dfa *d)
{
  o << *d;
  return o;
}

};

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
