build:
	bash ./Build/build.sh

run:
	bash ./Build/run.sh

deps-llvm:
	wget https://apt.llvm.org/llvm.sh
	chmod +x llvm.sh
	sudo ./llvm.sh 14 all

deps-debian: deps-llvm
	sudo apt update
	sudo apt install nasm xorriso build-essential qemu-system-x86 -y

clean:
	sudo rm -rf ./Bin ./Sources/Libs/libc/Lib/ ./Sources/Kernel/Lib/ ./Sources/Modules/Services/flowge/Lib/ ./Sources/Modules/Services/fs/Lib/ ./Sources/Modules/Services/system/Lib/ ./Sources/Modules/uisd/flowge/Lib/

github-action: deps-llvm build

.PHONY: build run llvm deps-debian github-action
