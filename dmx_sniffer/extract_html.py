with open(r'C:\DmxSnifer\dmx_sniffer\main\web_page.h', 'r', encoding='utf-8') as f:
    content = f.read()

start = content.index('R"rawliteral(') + len('R"rawliteral(')
end = content.index(')rawliteral"')
html = content[start:end].strip()

with open(r'C:\DmxSnifer\dmx_sniffer\data\index.html', 'w', encoding='utf-8') as f:
    f.write(html)

print(f"Written {len(html)} bytes")
