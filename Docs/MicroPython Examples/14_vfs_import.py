import bugbuster

# VFS is now enabled! 
# You can upload another file (e.g., 'mylib.py') and import it here.

print("--- VFS Import Example ---")

try:
    # Attempt to import a user-uploaded module
    import mylib
    print("Successfully imported mylib!")
    mylib.hello()
except ImportError:
    print("mylib.py not found on device.")
    print("Upload a file named 'mylib.py' using the Scripts tab to test this.")

# You can also use standard open() for text files in /spiffs/scripts/
try:
    with open('/spiffs/scripts/test_data.txt', 'w') as f:
        f.write('Hello VFS!')
    
    with open('/spiffs/scripts/test_data.txt', 'r') as f:
        print("Read from file:", f.read())
except Exception as e:
    print("File I/O error:", e)
