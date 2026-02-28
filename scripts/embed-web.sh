#!/bin/bash
# Embed web files into C source code

OUTPUT_FILE="$1"
INPUT_DIR="${2:-.}"

if [ -z "$OUTPUT_FILE" ]; then
    echo "Usage: $0 <output_file> [input_dir]"
    exit 1
fi

cat > "$OUTPUT_FILE" << 'EOF'
/* Auto-generated embedded web files. Do not edit. */

#include <stddef.h>

EOF

# Embed HTML
echo "Embedding web files into $OUTPUT_FILE..."

if [ -f "$INPUT_DIR/web/index.html" ]; then
    cat >> "$OUTPUT_FILE" << 'EOF'
const char web_index_html[] = {
EOF
    xxd -i < "$INPUT_DIR/web/index.html" >> "$OUTPUT_FILE"
    echo "};" >> "$OUTPUT_FILE"
    SIZE=$(wc -c < "$INPUT_DIR/web/index.html")
    echo "size_t web_index_html_size = $SIZE;" >> "$OUTPUT_FILE"
    echo "" >> "$OUTPUT_FILE"
fi

# Embed CSS
if [ -f "$INPUT_DIR/web/styles.css" ]; then
    cat >> "$OUTPUT_FILE" << 'EOF'
const char web_styles_css[] = {
EOF
    xxd -i < "$INPUT_DIR/web/styles.css" >> "$OUTPUT_FILE"
    echo "};" >> "$OUTPUT_FILE"
    SIZE=$(wc -c < "$INPUT_DIR/web/styles.css")
    echo "size_t web_styles_css_size = $SIZE;" >> "$OUTPUT_FILE"
    echo "" >> "$OUTPUT_FILE"
fi

# Embed JS
if [ -f "$INPUT_DIR/web/app.js" ]; then
    cat >> "$OUTPUT_FILE" << 'EOF'
const char web_app_js[] = {
EOF
    xxd -i < "$INPUT_DIR/web/app.js" >> "$OUTPUT_FILE"
    echo "};" >> "$OUTPUT_FILE"
    SIZE=$(wc -c < "$INPUT_DIR/web/app.js")
    echo "size_t web_app_js_size = $SIZE;" >> "$OUTPUT_FILE"
fi

echo "Done. Generated $OUTPUT_FILE"
