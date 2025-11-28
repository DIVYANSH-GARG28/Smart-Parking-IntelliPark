import easyocr
import cv2
import re
import requests

print("\nSelect Input Mode:")
print("1 → Webcam")
print("2 → Image File")
choice = input("Enter 1 or 2: ").strip()

use_camera = True if choice == "1" else False
image_path = "wel.jpg"  

esp_url = "http://10.133.159.212/ocr"

reader = easyocr.Reader(['en'], gpu=False)

def extract_plate_text(results):
    for (_, text, _) in results:
        clean = text.replace(" ", "").upper()
        if re.match(r"^[A-Z]{2}[0-9]{1,2}[A-Z]{1,3}[0-9]{4}$", clean):
            return clean
        if len(clean) >= 8 and any(c.isdigit() for c in clean):
            return clean
    return None

def correct_errors(plate):
    plate = plate.upper()
    if len(plate) >= 7:
        p = list(plate)
        for pos in [4, 5]:
            if pos < len(p):
                if p[pos] == "0": p[pos] = "Q"
                if p[pos] == "O": p[pos] = "Q"
        plate = "".join(p)
    return plate

def valid_plate(plate):
    pattern = r"^[A-Z]{2}[0-9]{1,2}[A-Z]{1,3}[0-9]{4}$"
    return re.match(pattern, plate)


def get_frame():
    if use_camera:
        cap = cv2.VideoCapture(0)
        if not cap.isOpened():
            print("Camera not found")
            return None

        print("Press 'q' to capture image")

        while True:
            ret, frame = cap.read()
            cv2.imshow("Camera Feed - Press q", frame)

            if cv2.waitKey(1) & 0xFF == ord('q'):
                cap.release()
                cv2.destroyAllWindows()
                return frame

    else:
        img = cv2.imread(image_path)
        
        if img is None:
            print("Image not found")
        return img


frame = get_frame()
if frame is None:
    print("No input available (camera/image).")
    requests.get(esp_url + "?plate=NONE&status=INVALID")
    exit()

results = reader.readtext(frame)
plate = extract_plate_text(results)

if not plate:
    print("Number Plate Not Visible ❌")
    # requests.get(f"{esp_url}?plate=NONE&status=INVALID")
    exit()

plate = correct_errors(plate)
print("Detected Plate:", plate)

if valid_plate(plate):
    print("Valid Plate ✓")
    requests.get(f"{esp_url}?plate={plate}&status=OK")
    with open("plate_log.txt", "a") as f:
        f.write(plate + "\n")
else:
    print("Invalid Plate ✗")
    # requests.get(f"{esp_url}?plate={plate}&status=INVALID")
