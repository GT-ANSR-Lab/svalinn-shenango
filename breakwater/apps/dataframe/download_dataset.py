#!/usr/bin/env python3

import pandas as pd
import glob
import requests
import os
import zipfile

#
# We will download the EUR/USD 1-min granularity Foreign Exchange dataset
#

# Create the data directory
os.makedirs("data", exist_ok=True)

# Download the dataset
url = "https://www.kaggle.com/api/v1/datasets/download/siddharth25/dataset-of-eurusd-1min-dataset-from-2018-to-2022"
response = requests.get(url)
with open("data/eur_usd_fx.zip", "wb") as f:
    f.write(response.content)

# Unzip the dataset
with zipfile.ZipFile("data/eur_usd_fx.zip", "r") as zip_ref:
    zip_ref.extractall("data")

# Combine all the excel worksheets into a single dataframe
orig_xlsx_file_list = glob.glob("data/*.xlsx")
orig_xlsx_file_list.sort()
# Consider only the first two files
xlsx_file_list = orig_xlsx_file_list[:2]

df_list = []
for xlsx_file in xlsx_file_list:
    df = pd.read_excel(xlsx_file, engine='openpyxl')
    df_list.append(df)
combined_df = pd.concat(df_list, ignore_index=True)
nb_rows = len(combined_df)

# Update the CSV headers to a typed format (as expected by DataFrame)
new_cols = []
for col in combined_df.columns:
    if "DateTime" in col:
        new_col = "INDEX:{}:<DateTime>".format(nb_rows)
    elif "OPEN" in col:
        new_col = "Open:{}:<double>".format(nb_rows)
    elif "HIGH" in col:
        new_col = "High:{}:<double>".format(nb_rows)
    elif "LOW" in col:
        new_col = "Low:{}:<double>".format(nb_rows)
    elif "CLOSE" in col:
        new_col = "Close:{}:<double>".format(nb_rows)
    elif "Volume" in col:
        new_col = "Volume:{}:<double>".format(nb_rows)
    new_cols.append(new_col)
combined_df.columns = new_cols

# Save the processed result in a CSV
combined_df.to_csv("data/eur_usd_fx.csv", index=False)

# Delete the intermediate files
os.remove("data/eur_usd_fx.zip")
for xlsx_file in orig_xlsx_file_list:
    os.remove(xlsx_file)
