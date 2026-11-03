"""Build the contributor-facing feature matrix from canonical coverage labels."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "progress.md"
LABELS = {
    "implemented": "🟢 Implemented within documented scope",
    "partial": "🟡 Partial",
    "missing": "⚪ Missing",
}


def heading_anchor(title):
    cleaned = re.sub(r"[^\w\- ]", "", title.lower())
    return cleaned.replace(" ", "-")


def render(coverage):
    features = coverage["features"]
    totals = Counter(feature["status"] for feature in features)
    lines = [
        "# Feature progress",
        "",
        "[← Project home](../README.md) · [Contribution workflow](guides/feature-workflow.md)",
        "",
        "This matrix reads its labels directly from `protocol/feature-coverage.json`.",
        "It does not estimate a completion percentage or promote packet coverage into gameplay.",
        "Open a feature to read its supported behavior, remaining limits, sources and evidence.",
        "",
        "## How to read the labels",
        "",
        "- **Implemented within documented scope:** the bounded emulator contract is implemented;",
        "  official parity, all content and original-client acceptance may still be limited.",
        "- **Partial:** meaningful behavior exists, but the feature has explicit remaining work.",
        "- **Missing:** no complete behavior-producing service is established for the feature.",
        "",
        f"**{len(features)} feature groups:** {totals['implemented']} bounded implementations, "
        f"{totals['partial']} partial and {totals['missing']} missing.",
        "These are feature-group labels, not player-readiness or test-coverage percentages.",
        "",
        "## Full feature matrix",
        "",
        "| Feature | Current label | Implementation and limits |",
        "| :--- | :--- | :--- |",
    ]
    for feature in features:
        anchor = heading_anchor(feature["title"])
        lines.append(
            f"| **{feature['title']}** | {LABELS[feature['status']]} | "
            f"[Read the feature](protocol-feature-reference.md#{anchor}) |"
        )
    lines.extend([
        "",
        "## Practical focus areas",
        "",
        "| Area | Supported foundation | Important remaining work |",
        "| :--- | :--- | :--- |",
        "| Login | Account sessions, launch grants, separate login/game admission | General platform authentication; client acceptance after the split |",
        "| Movement | Checkpoints, nearby movement, bounded tutorial travel | General portals/channels, navigation and broader world authority |",
        "| Combat | Server timing, costs/cooldowns, supported actions and effects | Full defence/effect behavior, broader combat and client acceptance |",
        "| Quests | Supported objectives, revisioned state and atomic rewards | Complete content and objective semantics; broader client playthroughs |",
        "| Gathering / crafting | Validated gathering sources, claims and supported processing | Expertise/level benefits and remaining authored policies/content |",
        "| Housing | Catalog-backed contracts, placed contents, permissions/storage | Complete ordinary-client contract/edit/placement/lifecycle acceptance |",
        "| Trade / marketplace | Bounded stateful trade and marketplace contracts | Official economic values and original-client workflow acceptance |",
        "| Social | Supported chat, friends, party and guild operations | Remaining audiences, travel, company behavior and persistence limits |",
        "",
        "These short descriptions are a reading aid. The linked canonical feature section",
        "takes precedence and contains details that a one-line summary cannot represent.",
        "",
        "## Evidence is separate from status",
        "",
        "Static recovery establishes a client contract; a capture establishes observed traffic;",
        "a synthetic check establishes the tested service/storage/transport behavior; an",
        "original-client trial establishes only the path and build actually observed.",
        "The latest split has synthetic two-process PostgreSQL/TLS verification, not a new",
        "original-client gameplay acceptance. See [validation](service-split-validation.md).",
        "",
        "## Updating this page",
        "",
        "Review the owning feature and evidence, update canonical coverage and regenerate:",
        "",
        "```powershell",
        "python tools/build_project_progress.py",
        "python tools/build_project_progress.py --check",
        "```",
        "",
        "This summary is generated. Update its generator when its reading guidance changes;",
        "update canonical coverage when the supported contract changes. Follow the full",
        "[reference maintenance procedure](protocol-reference-maintenance.md) for feature work.",
    ])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    coverage = json.loads((ROOT / "protocol/feature-coverage.json").read_text(encoding="utf-8"))
    document = render(coverage)
    if arguments.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != document:
            raise SystemExit("Regenerate docs/progress.md with tools/build_project_progress.py")
        print("Project progress matches canonical feature labels.")
        return

    OUTPUT.write_text(document, encoding="utf-8", newline="\n")
    print(f"Generated {len(coverage['features'])} feature groups.")


if __name__ == "__main__":
    main()
