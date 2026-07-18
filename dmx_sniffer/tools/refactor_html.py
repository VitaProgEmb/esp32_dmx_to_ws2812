"""Reformat web_page.h: split HTML into readable lines, fix CSS/status."""
import re

SRC = 'C:/DmxSnifer/dmx_sniffer/main/web_page.h'

with open(SRC, 'r', encoding='utf-8') as f:
    content = f.read()

start_marker = 'R"rawliteral('
end_marker = ')rawliteral"'

start = content.find(start_marker) + len(start_marker)
end = content.rfind(end_marker)
html = content[start:end]

# --- 1. Insert newlines after block/tag boundaries for readability ---
# Add newline after </style>
html = html.replace('</style>', '</style>\n')

# Add newline before <!doctype>, </head>, <head>, <body>, </body>, </html>
for tag in ['<!doctype', '<head>', '<style>', '</head>', '<body>', '</body>', '</html>']:
    html = html.replace(tag, '\n' + tag)

# Add newline before </div> (to help readability)
html = html.replace('</div>', '\n</div>')

# Add newline before <div
html = html.replace('<div ', '\n<div ')

# Add newline before <script> and </script>
html = html.replace('<script>', '\n<script>')
html = html.replace('</script>', '</script>\n')

# Clean up excessive blank lines
html = re.sub(r'\n\s*\n+', '\n', html)

# --- 2. Fix CSS: add .help class with word-wrap ---
# Find the </style> and insert .help class before it
help_css = '''
.help{line-height:1.8;word-wrap:break-word;overflow-wrap:break-word;overflow:hidden}
.ota-result{padding:6px 12px;border-radius:6px;font-weight:700;font-size:.95em;display:inline-block;margin-top:4px}
.ota-result.ok{background:#1a5c1a;color:#8f8}
.ota-result.err{background:#5c1a1a;color:#f88}
'''

style_end = html.rfind('</style>')
html = html[:style_end] + help_css + html[style_end:]

# --- 3. Replace otaStatus span with a more visible indicator ---
old_ota = '<span id="otaStatus" style="font-size:.85em;color:#aaa;margin-left:8px"></span>'
new_ota = '<span id="otaResult" class="ota-result"></span>'
html = html.replace(old_ota, new_ota)

# --- 4. Update uploadOTA JS: use otaResult instead of otaStatus ---
# Find uploadOTA function and update it
old_js_start = html.find('function uploadOTA()')
if old_js_start >= 0:
    # Find the function end
    body_start = html.find('{', old_js_start)
    depth = 0
    i = body_start
    while i < len(html):
        if html[i] == '{': depth += 1
        elif html[i] == '}': depth -= 1
        if depth == 0:
            old_js = html[old_js_start:i+1]
            break
        i += 1
    
    new_js = """function uploadOTA(){
    var f=document.getElementById("otaFile").files[0];
    if(!f)return;
    var r=document.getElementById("otaResult");
    var p=document.getElementById("otaProgress");
    r.textContent="Загрузка...";r.className="ota-result";
    p.value=0;
    var x=new XMLHttpRequest;
    x.open("POST","/api/ota");
    x.upload.onprogress=function(e){
        e.lengthComputable&&(p.value=Math.round(e.loaded/e.total*100))
    };
    x.onload=function(){
        try{
            var j=JSON.parse(x.responseText);
            if(j.ok){r.textContent="OK, перезагрузка...";r.className="ota-result ok"}
            else{r.textContent="Ошибка: "+(j.error||"?");r.className="ota-result err"}
        }catch(e){
            r.textContent="Ошибка: "+x.status;r.className="ota-result err"
        }
    };
    x.onerror=function(){r.textContent="Ошибка сети";r.className="ota-result err"};
    x.send(f)
}"""
    html = html[:old_js_start] + new_js + html[body_start+1:]

# Recompute start/end since we may have shifted things
# Actually start/end haven't changed because we replaced within the HTML

# --- 5. Write back ---
new_content = content[:start] + html + content[end:]
with open(SRC, 'w', encoding='utf-8') as f:
    f.write(new_content)

print("web_page.h updated successfully.")
print(f"HTML size: {len(html)} chars")
