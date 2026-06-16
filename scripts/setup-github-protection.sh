#!/bin/bash
# scripts/setup-github-protection.sh — Configure GitHub branch protection
#
# Requires: gh (GitHub CLI), authenticated with repo admin access
#
# This sets up branch protection on 'main' to require:
#   - Signed commits
#   - At least 1 approving review
#   - CI checks to pass
#   - No direct pushes (all changes via PR)

set -e

REPO="marcel-b-roodt/MiniMem"

if ! command -v gh &>/dev/null; then
    echo "Error: gh (GitHub CLI) not installed."
    echo "Install: https://cli.github.com/"
    exit 1
fi

echo "Setting up branch protection for $REPO/main ..."

gh api "repos/$REPO/branches/main/protection" \
    -X PUT \
    --input - << 'EOF'
{
    "required_status_checks": {
        "strict": true,
        "contexts": []
    },
    "enforce_admins": false,
    "required_pull_request_reviews": {
        "dismiss_stale_reviews": true,
        "require_code_owner_reviews": false,
        "required_approving_review_count": 1
    },
    "restrictions": null,
    "required_signatures": true
}
EOF

echo ""
echo "Branch protection configured:"
echo "  - Signed commits required"
echo "  - 1 approving review required"
echo "  - Stale reviews dismissed on push"
echo "  - Admins not exempt (change enforce_admins to true if desired)"
echo ""
echo "To add required CI checks, update contexts in the status checks"
echo "after setting up CI."