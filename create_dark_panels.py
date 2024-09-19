import os

# Define the dictionary with word replacements
word_replacements = {
  "#909090": "#252626",
  "#ab5b87": "#79305a",
  '#ecedf1': '#f7c5ad',
  '#fff': '#f7c5ad',
}

# Get the path to the res directory
res_directory = "./res"

# Iterate over all files in the res directory
for filename in os.listdir(res_directory):
  if filename.endswith(".svg") and not filename.endswith("-dark.svg"):
    # Read the contents of the file
    with open(os.path.join(res_directory, filename), "r") as file:
      file_contents = file.read()

    # Perform word replacements
    for old_word, new_word in word_replacements.items():
      file_contents = file_contents.replace(old_word, new_word)

    # Write the modified contents to a new file with '-dark' prefix
    new_filename = filename.replace(".svg", "-dark.svg")
    with open(os.path.join(res_directory, new_filename), "w") as file:
      file.write(file_contents)