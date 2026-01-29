#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_fastbot_logs.py

Analyze Fastbot native structured logs and visualize:
- [STATE] events: state id, naming hash, fineness, widget/action counts, trivial flag
- [NAMING] events: dynamic abstraction/refinement of naming
- [REFINE]/[ABSTRACT] detail events
- [REBUILD] events: graph size before/after rebuild

Usage:
  adb logcat -d -s FastbotNative > fastbot.log
  python3 analyze_fastbot_logs.py fastbot.log --out-prefix run1
"""

import argparse
import re
from dataclasses import dataclass
from typing import Dict, List, Optional

try:
    import matplotlib.pyplot as plt

    HAS_MPL = True
except ImportError:
    HAS_MPL = False


# ===== Data classes =====

@dataclass
class StateEvent:
    index: int          # log line index (1-based)
    state_id: str
    naming: int
    fineness: int
    widgets: int
    actions: int
    trivial: bool


@dataclass
class NamingEvent:
    index: int
    event: str          # refine_over / refine_ndet / abstract / abstract_detail / refine_*
    state_id: Optional[str]  # may be None for some abstract events
    old_naming: int
    new_naming: int
    fineness_old: int
    fineness_new: int
    extra: Dict[str, str]


@dataclass
class RebuildEvent:
    index: int
    states_before: int
    states_after: int
    transitions_before: int
    transitions_after: int


# ===== Regex patterns =====

STATE_RE = re.compile(
    r"\[STATE\]\s+id=(\S+)\s+"
    r"naming=(\d+)\s+"
    r"fineness=(\d+)\s+"
    r"widgets=(\d+)\s+"
    r"actions=(\d+)\s+"
    r"trivial=(\d+)"
)

NAMING_RE = re.compile(
    r"\[NAMING\]\s+event=(\w+)\s+"
    r"(?:state=(\S+)\s+)?"
    r"old=(\d+)\s+new=(\d+)\s+"
    r"fineness_old=(\d+)\s+fineness_new=(\d+)"
)

REFINE_RE = re.compile(
    r"\[REFINE\]\s+cause=(\w+)\s+state=(\S+)\s+"
    r"targetHash=(\d+)\s+"
    r"naming_old=(\d+)\s+naming_new=(\d+)\s+"
    r"fineness_old=(\d+)\s+fineness_new=(\d+)\s+"
    r"aliasedCount=(\d+)"
)

ABSTRACT_RE = re.compile(
    r"\[ABSTRACT\]\s+"
    r"naming_old=(\d+)\s+naming_new=(\d+)\s+"
    r"fineness_old=(\d+)\s+fineness_new=(\d+)\s+"
    r"affectedStates=(\d+)\s+distinctKeys=(\d+)"
)

REBUILD_RE = re.compile(
    r"\[REBUILD\]\s+"
    r"states_before=(\d+)\s+states_after=(\d+)\s+"
    r"transitions_before=(\d+)\s+transitions_after=(\d+)"
)


# ===== Parsing =====

def parse_log(path: str):
    state_events: List[StateEvent] = []
    naming_events: List[NamingEvent] = []
    rebuild_events: List[RebuildEvent] = []
    refine_events: List[NamingEvent] = []
    abstract_detail_events: List[NamingEvent] = []

    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for idx, line in enumerate(f, start=1):
            line = line.strip()

            m = STATE_RE.search(line)
            if m:
                state_id, naming, fineness, widgets, actions, trivial = m.groups()
                state_events.append(
                    StateEvent(
                        index=idx,
                        state_id=state_id,
                        naming=int(naming),
                        fineness=int(fineness),
                        widgets=int(widgets),
                        actions=int(actions),
                        trivial=(trivial == "1"),
                    )
                )
                continue

            m = NAMING_RE.search(line)
            if m:
                event, state_id, old_n, new_n, f_old, f_new = m.groups()
                naming_events.append(
                    NamingEvent(
                        index=idx,
                        event=event,
                        state_id=state_id,
                        old_naming=int(old_n),
                        new_naming=int(new_n),
                        fineness_old=int(f_old),
                        fineness_new=int(f_new),
                        extra={},
                    )
                )
                continue

            m = REFINE_RE.search(line)
            if m:
                cause, state_id, target_hash, old_n, new_n, f_old, f_new, aliased = m.groups()
                refine_events.append(
                    NamingEvent(
                        index=idx,
                        event=f"refine_{cause}",
                        state_id=state_id,
                        old_naming=int(old_n),
                        new_nming=int(new_n),
                        fineness_old=int(f_old),
                        fineness_new=int(f_new),
                        extra={
                            "targetHash": target_hash,
                            "aliasedCount": aliased,
                        },
                    )
                )
                continue

            m = ABSTRACT_RE.search(line)
            if m:
                old_n, new_n, f_old, f_new, aff, keys = m.groups()
                abstract_detail_events.append(
                    NamingEvent(
                        index=idx,
                        event="abstract_detail",
                        state_id=None,
                        old_nming=int(old_n),
                        new_nming=int(new_n),
                        fineness_old=int(f_old),
                        fineness_new=int(f_new),
                        extra={
                            "affectedStates": aff,
                            "distinctKeys": keys,
                        },
                    )
                )
                continue

            m = REBUILD_RE.search(line)
            if m:
                sb, sa, tb, ta = m.groups()
                rebuild_events.append(
                    RebuildEvent(
                        index=idx,
                        states_before=int(sb),
                        states_after=int(sa),
                        transitions_before=int(tb),
                        transitions_after=int(ta),
                    )
                )
                continue

    return state_events, naming_events, refine_events, abstract_detail_events, rebuild_events


# ===== Summary =====

def summarize(
    states: List[StateEvent],
    namings: List[NamingEvent],
    refines: List[NamingEvent],
    abstracts_detail: List[NamingEvent],
    rebuilds: List[RebuildEvent],
):
    print("=== Summary ===")
    print(f"Total [STATE] events: {len(states)}")
    print(f"Total [NAMING] events: {len(namings)}")
    print(f"Total [REFINE] detail events: {len(refines)}")
    print(f"Total [ABSTRACT] detail events: {len(abstracts_detail)}")
    print(f"Total [REBUILD] events: {len(rebuilds)}")
    print()

    fineness_by_naming: Dict[int, List[int]] = {}
    for s in states:
        fineness_by_naming.setdefault(s.naming, []).append(s.fineness)

    print("=== Naming fineness overview (from [STATE]) ===")
    for n, fins in sorted(fineness_by_naming.items(), key=lambda kv: kv[0])[:20]:
        fins_sorted = sorted(fins)
        median = fins_sorted[len(fins_sorted) // 2]
        print(
            f"naming={n}: count={len(fins)}, "
            f"fineness(min/median/max)={fins_sorted[0]}/{median}/{fins_sorted[-1]}"
        )
    if len(fineness_by_naming) > 20:
        print(f"... ({len(fineness_by_naming)} total namings, showing first 20)")
    print()

    print("=== Recent [NAMING] events ===")
    for e in namings[-10:]:
        print(
            f"#{e.index} event={e.event} state={e.state_id or '-'} "
            f"{e.old_nming}->{e.new_nming} fineness {e.fineness_old}->{e.fineness_new}"
        )
    print()

    if refines:
        print("=== Recent [REFINE] events (aliased / other causes) ===")
        for e in refines[-10:]:
            print(
                f"#{e.index} cause={e.event} state={e.state_id} "
                f"naming {e.old_nming}->{e.new_nming} "
                f"fineness {e.fineness_old}->{e.fineness_new} "
                f"aliased={e.extra.get('aliasedCount')} target={e.extra.get('targetHash')}"
            )
        print()

    if abstracts_detail:
        print("=== Recent [ABSTRACT] detail events ===")
        for e in abstracts_detail[-10:]:
            print(
                f"#{e.index} naming {e.old_nming}->{e.new_nming} "
                f"fineness {e.fineness_old}->{e.fineness_new} "
                f"affectedStates={e.extra.get('affectedStates')} "
                f"distinctKeys={e.extra.get('distinctKeys')}"
            )
        print()

    if rebuilds:
        print("=== [REBUILD] events ===")
        for r in rebuilds:
            print(
                f"#{r.index} states {r.states_before}->{r.states_after}, "
                f"transitions {r.transitions_before}->{r.transitions_after}"
            )
        print()


# ===== Plotting =====

def plot_events(
    states: List[StateEvent],
    namings: List[NamingEvent],
    rebuilds: List[RebuildEvent],
    output_prefix: str,
):
    if not HAS_MPL:
        print("matplotlib not installed, skip plotting. Run `pip install matplotlib` to enable.")
        return

    if states:
        plt.figure(figsize=(10, 5))
        xs = [s.index for s in states]
        ys = [s.fineness for s in states]
        plt.scatter(xs, ys, s=8, alpha=0.4, label="state fineness")

        for ne in namings:
            color_map = {
                "refine_over": "red",
                "refine_ndet": "orange",
                "abstract": "blue",
                "abstract_detail": "blue",
            }
            color = color_map.get(ne.event, "green")
            plt.axvline(ne.index, color=color, alpha=0.3, linewidth=1)

        plt.xlabel("log line index")
        plt.ylabel("fineness (namer granularity)")
        plt.title("Naming fineness over time")
        plt.legend()
        plt.tight_layout()
        out1 = f"{output_prefix}_fineness.png"
        plt.savefig(out1, dpi=150)
        print(f"[SAVE] {out1}")

    if rebuilds:
        plt.figure(figsize=(8, 4))
        xs = [r.index for r in rebuilds]
        before = [r.states_before for r in rebuilds]
        after = [r.states_after for r in rebuilds]
        plt.plot(xs, before, "o--", label="states_before")
        plt.plot(xs, after, "s-", label="states_after")
        plt.xlabel("log line index")
        plt.ylabel("state count")
        plt.title("State count before/after REBUILD events")
        plt.legend()
        plt.tight_layout()
        out2 = f"{output_prefix}_rebuild_states.png"
        plt.savefig(out2, dpi=150)
        print(f"[SAVE] {out2}")

    plt.close("all")


# ===== CLI entry point =====

def main():
    parser = argparse.ArgumentParser(description="Analyze Fastbot native structured logs.")
    parser.add_argument("logfile", help="Path to fastbot log file (e.g., fastbot.log)")
    parser.add_argument("--out-prefix", default="fastbot_analysis", help="Prefix for output plots")
    args = parser.parse_args()

    states, namings, refines, abstracts_detail, rebuilds = parse_log(args.logfile)
    summarize(states, namings, refines, abstracts_detail, rebuilds)

    all_naming_events: List[NamingEvent] = []
    all_naming_events.extend(namings)
    all_naming_events.extend(refines)
    all_naming_events.extend(abstracts_detail)

    plot_events(states, all_naming_events, rebuilds, args.out_prefix)


if __name__ == "__main__":
    main()

