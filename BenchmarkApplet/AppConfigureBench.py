from ast import arguments
import os
import shutil
import itertools


def remove_old_copies():
    print("Removing old copies of executable file and data directory")
    if os.path.exists("./NeuralRadianceCaching.exe"):
        os.remove("./NeuralRadianceCaching.exe")
    if os.path.exists("./glfw3.dll"):
        os.remove("./glfw3.dll")
    if os.path.exists("./data"):
        shutil.rmtree("./data")


def copy_executable_file():
    print("Copying the executable file")
    shutil.copy("../cmake-build-default/NeuralRadianceCaching.exe", ".")


def copy_dlls():
    print("Copying required dll's for executable")
    shutil.copy("../cmake-build-default/glfw3.dll", ".")


def copy_data_dir():
    print("Copying the data folder")
    shutil.copytree("../data", "./data")


def update_file_hierarchy():
    remove_old_copies()
    copy_executable_file()
    copy_dlls()
    copy_data_dir()


def generate_configs():
    print("Generating app configs for NeuralRadianceCache Renderer.exe")

    loss_fn_options = ["RelativeL2"]
    optimizer_options = ["Adam"]
    learning_rate_options = ["0.01", "0.001", "0.0001", "0.00001"]
    encoding_options = ["0"]
    nn_width_options = ["64", "128"]
    nn_depth_options = ["2", "4", "6", "8", "10"]
    log2_batch_size_options = ["12", "13", "14", "15", "16"]
    scene_options = ["0"]
    render_width_options = ["1920"]
    render_height_options = ["1080"]
    train_sample_ratio_options = ["0.01", "0.02", "0.05", "0.1"]
    train_spp_options = ["1", "2", "4", "8"]

    arguments_list = list(itertools.product(
        loss_fn_options,
        optimizer_options,
        learning_rate_options,
        encoding_options,
        nn_width_options,
        nn_depth_options,
        log2_batch_size_options,
        scene_options,
        render_width_options,
        render_height_options,
        train_sample_ratio_options,
        train_spp_options))
    arguments_list_len = len(arguments_list)
    print(str(arguments_list_len)+ "configs created")

    configs_file = open("configs.csv", "w")

    for index, arguments in enumerate(arguments_list):
        arguments_str = ""
        for arg in arguments:
            arguments_str += str(arg) + " "
        arguments_list[index] = arguments_str
        configs_file.write(arguments_str + "\n")
        # print(str(index) + ": " + str(arguments_str))

    configs_file.close()

def execute_config(index, arguments):
    print("Executing config " + str(index) + ": " + str(arguments))
    os.system("NeuralRadianceCaching.exe " + arguments)

def execute_configs():
    print("Executing app configs")
    config_file = open("configs.csv", "r")
    arguments_list = config_file.readlines()
    config_file.close()

    print(str(len(arguments_list)) + " configs loaded")

    for index, arguments in enumerate(arguments_list):
        arguments = arguments.strip()
        arguments_list[index] = arguments
        execute_config(index, arguments)


def evaluate_results():
    print("Evaluating results")


def main():
    print("Starting AppConfigureBench")
    update_file_hierarchy()
    generate_configs()
    execute_configs()
    evaluate_results()


if __name__ == "__main__":
    main()
