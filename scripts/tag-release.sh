#!/bin/bash

# Help function
print_help() {
    echo "Usage: $0 <TAG_NAME> <COMMIT_HASH>"
    echo ""
    echo "Creates a Git tag for a specific commit and pushes it to the remote repository."
    echo ""
    echo "Arguments:"
    echo "  TAG_NAME       The name of the tag to create (e.g., v1.0.7)"
    echo "  COMMIT_HASH    The specific commit hash to tag"
    echo ""
    echo "Options:"
    echo "  -h, --help     Display this help message and exit"
}

# Check for help flag
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    print_help
    exit 0
fi

# Check if both arguments are provided
if [[ -z "$1" || -z "$2" ]]; then
    echo "Error: Missing required arguments." >&2
    echo ""
    print_help
    exit 1
fi

TAG_NAME="$1"
COMMIT_HASH="$2"
REMOTE="origin"

echo "Creating tag '$TAG_NAME' for commit $COMMIT_HASH..."

# Create the annotated tag and push it only if both are provided
if git tag -a "$TAG_NAME" -m "Release $TAG_NAME" "$COMMIT_HASH" && git push "$REMOTE" "$TAG_NAME"; then
    echo "Successfully created and pushed tag '$TAG_NAME' to $REMOTE!"
else
    echo "Error: Failed to create or push the tag." >&2
    exit 1
fi