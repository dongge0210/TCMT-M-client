import urllib.request
from html.parser import HTMLParser

class LinkParser(HTMLParser):
    def handle_starttag(self, tag, attrs):
        if tag == 'a':
            for attr, value in attrs:
                if attr == 'href' and 'listbox' in value.lower():
                    print(f"Found link: {value}")

try:
    with urllib.request.urlopen("https://docs.avaloniaui.net/docs/basics/user-interface/controls/listbox") as response:
        html = response.read().decode('utf-8')
        print(html[:500]) # Print snippet to see if we hit the right page
except Exception as e:
    print(f"Error: {e}")
