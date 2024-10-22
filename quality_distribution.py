import csv
import sys

max_int = sys.maxsize
while True:
    try:
        csv.field_size_limit(max_int)
        break
    except OverflowError:
        max_int = int(max_int/10)

file = sys.argv[1] if len(sys.argv) > 1 else input("Please enter the .csv data file: ")

def compute(values, out):
    res = list(sorted(map(lambda x: tuple(map(int, x.split(';'))), values.split('|')[:-1])))
    cum = [None] * len(res)
    total = sum(map(lambda x: x[1], res))
    running_total = 0
    out.write("err cum\n")
    for i in range(len(res)):
        out.write(str(res[i][0]) + " " + str(float(total - running_total) / total) + "\n")
        cum[i] = float(total - running_total) / total
        running_total += res[i][1]
    out.write("\n")

with open(file) as file:
    lines = csv.reader(file)
    for row in lines:
        with open(row[0] + "_rank_error.txt", "w") as out:
            compute(row[5], out)
        with open(row[0] + "_delay.txt", "w") as out:
            compute(row[5], out)