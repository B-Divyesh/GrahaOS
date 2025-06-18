#!/usr/bin/env bash

# Script to copy all project files to textfiles directory with .txt extension
# Ignores env, limine, and toolchain directories

set -e

# Create output directory
mkdir -p textfiles

# Function to add appropriate comment based on file extension
add_comment_and_copy() {
    local src_file="$1"
    local dest_file="$2"
    local orig_ext="${src_file##*.}"
    local comment_prefix=""
    
    # Set comment prefix based on file extension
    case "$orig_ext" in
        c|h|cpp|hpp) comment_prefix="//" ;;
        S|s|asm) comment_prefix=";" ;;
        sh|bash) comment_prefix="#" ;;
        py) comment_prefix="#" ;;
        *) comment_prefix="#" ;; # Default for unknown types
    esac
    
    # Create the destination file with comment header and original content
    echo "${comment_prefix} Original location: ${src_file}" > "$dest_file"
    echo "${comment_prefix} Original extension: ${orig_ext}" >> "$dest_file"
    echo "" >> "$dest_file"
    cat "$src_file" >> "$dest_file"
    
    echo "Copied: $src_file -> $dest_file"
}

# Find all files, excluding directories to ignore
find . -type f \
    -not -path "*/\.*" \
    -not -path "*/env/*" \
    -not -path "*/limine/*" \
    -not -path "*/toolchain/*" \
    -not -path "*/textfiles/*" \
    -not -path "*/copy_to_textfiles.sh" \
    | while read -r file; do
        # Generate a unique filename for the destination
        filename=$(basename "$file" | tr -c '[:alnum:]._-' '_')
        dest_file="textfiles/${filename}.txt"
        
        # If a file with the same name exists, add a suffix
        if [ -f "$dest_file" ]; then
            counter=1
            while [ -f "textfiles/${filename}_${counter}.txt" ]; do
                counter=$((counter + 1))
            done
            dest_file="textfiles/${filename}_${counter}.txt"
        fi
        
        add_comment_and_copy "$file" "$dest_file"
    done

echo "All files have been copied to the textfiles directory with .txt extension."