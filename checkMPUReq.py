#!/usr/bin/python

import sys, getopt
import math
 
# Function to check
# Log base 2
def Log2(x):
	return (math.log10(x) /
			math.log10(2));
def isPowerOfTwo(n):
	return (math.ceil(Log2(n)) == math.floor(Log2(n)));
sizeRegionMap= { 0:"0", 32: "REGION_32B", 64: "REGION_64B", 128: "REGION_128B", 256: "REGION_256B", 512: "REGION_512B", 1024: "REGION_1K", 2048: "REGION_2K", 4096: "REGION_4K", 16384: "REGION_16K", 32768: "REGION_32K", 65536: "REGION_64K", 131072: "REGION_128K", 262144: "REGION_256K", 524288: "REGION_512K", 1048576: "REGION_1M", 2097152:"REGION_2M", 4194304:"REGION_4M",8388608:"REGION_8M",16777216:"REGION_16M",33554432:"REGION_32M",67108864:"REGION_64M",134217728:"REGION_128M",268435456:"REGION_256M",536870912:"REGION_512M",1073741824:"REGION_1G",2147483648:"REGION_2G",4294967296:"REGION_4G"}
	

def valid(base, size):
	if size != 0:
		upSize = size
		print("Is power of two?" + str(isPowerOfTwo(size)))
		if not isPowerOfTwo(size):
				upSize = 2 ** math.ceil(Log2(size))
				print(upSize - size)
		size = upSize 
		if base %size:
			print("Target base address is not divisible!")
			div = base/size
			div +=1
			newOffset = size *div
			print(newOffset)
		if size<32:
			print("Is less than 32 bytes")
def writeCodeSections(cpatch, csections):
	i = 0
	for section in sorted(csections):
				cpatch.write("  . = "+ str(csections[section][1]) +";\n")
				cpatch.write("  .csection"+str(i) +" : \n")
				cpatch.write("  {\n")
				cpatch.write("_scsection"+str(i)+" = .;\n")
				cpatch.write("	*(.csection"+str(i)+"*)\n")
				cpatch.write("	. = "+ str(csections[section][0] + csections[section][1])+";\n")
				cpatch.write("_ecsection"+str(i)+" = .;\n")
				cpatch.write("  } > FLASH \n")
				i +=1

def writeDataSections(dpatch, dsections):
	i =0
	for section in sorted(dsections):
				dpatch.write("  .osection"+str(i) +" : AT ( _sidata  + compartLMA)\n")
				dpatch.write("  {\n")
				dpatch.write("	. = "+ str(dsections[section][1])+";\n")
				dpatch.write("	_sosection" +str(i) +" = .;\n")
				dpatch.write("	*(.osection"+str(i)+"*)\n")
				dpatch.write("	. = "+ str(dsections[section][0] + dsections[section][1])+";\n")
				dpatch.write("	_eosection" +str(i) +" = .;\n")
				dpatch.write("  }  > RAM \n")
#				dpatch.write("  }  > RAM \n")
				dpatch.write("compartLMA = compartLMA + SIZEOF(.osection"+str(i) +"); \n")
				i += 1
	dpatch.write("_edata = .; \n")


def fixup(section):
		if section[0] < 32:
			section[0] = 32
		upSize = 2 ** math.ceil(Log2(section[0]))
		section[0] = int(upSize)
		if section[1] %section[0]:
				div = section[1]/section[0]
				div += 1
				print ("Update from: " + str(section[1])+ " to:"+ str(section[0] * div))
				section[1] = int(section[0] * div)
		print(section)
		return section[0]+section[1]

def updateSize(codesections):
	it = sorted(codesections)
	for csec in sorted(codesections):
			[size,base] = codesections[csec]
			ind = it.index(csec)
			ind +=1
			if ind < len(it):
				nextSec=it[ind]
				[nsize,nbase] = codesections[nextSec]
				sizePad = nbase - base
				if not sizePad == size:
					print ("Updating size from" + str(size) + " to " + str(sizePad))
					codesections[csec][0] = sizePad

def printSortedAndFixupSections(sections, start):
	lc = start
	for elem in sorted(sections):
		print(elem + ":")
		print(sections[elem])
		if not lc == -1: 
			sections[elem][1] = lc 
		if not sections[elem][0] == 0:
			valid(sections[elem][1],sections[elem][0])
			lc = fixup(sections[elem]) #Location counter tells from where the next section base should begin.
			print(sections[elem])

def printSortedAndVerifSections(sections):
	for elem in sorted(sections):
		 print(elem + ":")
		 print(sections[elem])
		 if not sections[elem][0] == 0:
				valid(sections[elem][1],sections[elem][0])
	

def main(argv):
	inputfile = ''
	outputfile = ''
	try:
		opts, args = getopt.getopt(argv,"hi:o:l:c:",["ifile=","ofile="])
	except getopt.GetoptError:
		print 'test.py -i <inputfile> -o <outputfile>'
		sys.exit(2)
	for opt, arg in opts:
		if opt == '-h':
			print 'test.py -i <inputfile> -o <outputfile>'
			sys.exit()
		elif opt in ("-i", "--ifile"):
			inputfile = arg
		elif opt in ("-l"):
			overlay = arg
		elif opt in ("-c"):
			configFile = arg
	print 'Input file is "', inputfile
	outputFile = overlay.replace("overlay","ld")
	outFile = open(outputFile, "w")
	secinfo = {}
	init = []
	rtmkCode = []
	rtmkData = []
	FLASH_BASE = 0x8000000
	RAM_BASE = 0x20000000
	with open(inputfile) as f:
			lines = f.readlines()
			for line in lines:
				line = line.replace("\n","")
				
				if ".rtmkcode" in line:
					for word in line.split(" "):
							word = unicode(word, "utf-8")
							if word.isdecimal():
									num = int(word)
									rtmkCode.append(num)
				if ".rtmkdata" in line:
					for word in line.split(" "):
							word = unicode(word, "utf-8")
							if word.isdecimal():
									num = int(word)
									rtmkData.append(num)

				if "csection" in line or "osection" in line:
					secinfo[line.split(" ")[0]] = []
					for word in line.split(" "):
							word = unicode(word, "utf-8")
							if word.isdecimal():
									num = int(word)
									secinfo[line.split(" ")[0]].append(num)

	print(secinfo)

	# MPU Requirements dictate that 
	#	1. size is power of 2
	#	2. size is more than 32 bytes
	#	3. base address is multiple of size.
	#
	codesections = {}
	datasections = {}
	for section in secinfo:
		[size, base] = secinfo[section]

		if ("csection" in section):
			codesections[section] = [size, base]
		else:
		 	datasections[section] = [size, base]

	lumpText = [rtmkCode[0]+rtmkCode[1] - FLASH_BASE, FLASH_BASE]
	printSortedAndFixupSections(codesections, fixup(lumpText))
	print("LumpTexzt:************")
	print(lumpText)
	lumpData = [rtmkData[0]+rtmkData[1] - RAM_BASE, RAM_BASE]
	printSortedAndFixupSections(datasections, fixup(lumpData))
	print("LumpData:*************")
	print(lumpData)
	

	print("********************************")
	printSortedAndVerifSections(codesections)
	
	outputFile = overlay.replace("overlay","ld")
	outFile = open(outputFile, "w")
	with open(overlay) as f:
		lines = f.readlines()
		for line in lines:
			if("datamarker-fixup::" in line):
					writeDataSections(outFile, datasections)
			elif("codemarker-fixup::" in line):
					writeCodeSections(outFile, codesections)
			else:
					outFile.write(line)
	prologue_string  = "#include <rtmk.h> \n RTMK_DATA \n  SEC_INFO comp_info[] = {"
	#CodeStart,CodeSize,DataStart,DataSize
	endstring = "}; \n"
	f = open(configFile, "w")
	f.write(prologue_string)
	print(datasections)
	i =0 
	for section in sorted(codesections):
			dsection = section
			f.write("/*"+ section + "*/")
			dsection = ".o" + section[2:]
			dsection = datasections[dsection]
			csection = codesections[section]
			f.write("{")
			f.write(str(csection[1]))#Offset
			f.write(",")
			print(csection[0])
			f.write(sizeRegionMap[csection[0]])#Size
			f.write(",")
			f.write(str(dsection[1]))
			f.write(",")
			print(dsection[0])
			f.write(sizeRegionMap[dsection[0]])
			f.write("}")
			i +=1
			if i!= len(codesections):
					f.write(",")
	f.write(endstring)
	f.write("int code_base= "+ str(FLASH_BASE) + ";\n");
	f.write("int code_size= "+ str(sizeRegionMap[codesections[".csection0"][1] - FLASH_BASE]) +";\n")
	f.write("int data_base= "+ str(RAM_BASE) + ";\n");
	f.write("int data_size= "+ str(sizeRegionMap[datasections[".osection0"][1] - RAM_BASE]) + ";\n")



if __name__ == "__main__":
	main(sys.argv[1:])
