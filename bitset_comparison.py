import csv
import sys
import statistics

file = sys.argv[1] if len(sys.argv) > 1 else input("Please enter the .csv data file: ")

values = { }

with open(file) as file:
    lines = csv.reader(file)
    for row in lines:
        values.setdefault(row[1], {}).setdefault(row[0], []).append(float(row[3]))

print(values)
for n, v in values.items():
    print(n)
    print("label bitsize its std")
    for bitsize, a in v.items():
        print(n + " " + bitsize + " " + str(statistics.mean(a)) + " " + str(statistics.stdev(a)))
    print()

