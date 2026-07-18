"""Extract and reformat HTML from web_page.h"""
import sys

def main():
    with open('C:/DmxSnifer/dmx_sniffer/main/web_page.h', 'r', encoding='utf-8') as f:
        content = f.read()

    start_marker = 'R"rawliteral('
    end_marker = ')rawliteral"'

    start = content.find(start_marker) + len(start_marker)
    end = content.rfind(end_marker)
    html = content[start:end]

    print(f"HTML length: {len(html)} chars")
    print("First 200 chars:")
    print(html[:200])
    print()
    print("Last 200 chars:")
    print(html[-200:])
    
    # Check for help section
    import re
    help_idx = html.find('Справка по DMX512')
    if help_idx >= 0:
        print(f"\nHelp section at index {help_idx}:")
        print(html[help_idx-300:help_idx+300])

if __name__ == '__main__':
    main()
