// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Diagnostics;
using System.Text;

#nullable enable

namespace Wasm.Build.Tests
{
    public static class HelperExtensions
    {
        public static IEnumerable<object?[]> UnwrapItemsAsArrays(this IEnumerable<IEnumerable<object?>> enumerable)
            => enumerable.Select(e => e.ToArray());

        public static IEnumerable<object?[]> Dump(this IEnumerable<object?[]> enumerable)
        {
            foreach (var row in enumerable)
            {
                Console.WriteLine ("{");
                foreach (var param in row)
                    Console.WriteLine ($"\t{param}");
                Console.WriteLine ("}");
            }
            return enumerable;
        }

        /// <summary>
        /// Cartesian product
        ///
        /// Say we want to provide test data for:
        ///     [MemberData(nameof(TestData))]
        ///     public void Test(string name, int num) { }
        ///
        /// And we want to test with `names = object[] { "Name0", "Name1" }`
        ///
        /// And for each of those names, we want to test with some numbers,
        ///   say `numbers = object[] { 1, 4 }`
        ///
        /// So, we want the final test data to be:
        ///
        ///     { "Name0", 1 }
        ///     { "Name0", 4 }
        ///     { "Name1", 1 }
        ///     { "Name1", 4 }
        ///
        /// Then we can use: names.Combine(numbers)
        ///
        /// </summary>
        /// <param name="data"></param>
        /// <param name="rowsWithColumnArrays"></param>
        /// <returns></returns>
        public static IEnumerable<IEnumerable<object?>> Multiply(this IEnumerable<IEnumerable<object?>> data, params object?[][] rowsWithColumnArrays)
            => data.SelectMany(row =>
                        rowsWithColumnArrays.Select(new_cols => row.Concat(new_cols)));

        public static IEnumerable<IEnumerable<object?>> MultiplyWithSingleArgs(this IEnumerable<IEnumerable<object?>> data, params object?[] arrayOfArgs)
            => data.SelectMany(row => arrayOfArgs.Select(argCol => row.Concat(new[] { argCol })));

        public static void UpdateTo(this IDictionary<string, (string fullPath, bool unchanged)> dict, bool unchanged, params string[] filenames)
        {
            IEnumerable<string> keys = filenames.Length == 0 ? dict.Keys.ToList() : filenames;

            foreach (var filename in keys)
            {
                if (!dict.TryGetValue(filename, out var oldValue))
                {
                    StringBuilder sb = new();
                    sb.AppendLine($"Cannot find key named {filename} in the dict. Existing ones:");
                    foreach (var kvp in dict)
                        sb.AppendLine($"[{kvp.Key}] = [{kvp.Value}]");

                    throw new KeyNotFoundException(sb.ToString());
                }

                dict[filename] = (oldValue.fullPath, unchanged);
            }
        }

        public static ProcessStartInfo RemoveEnvironmentVariables(this ProcessStartInfo psi, params string[] names)
        {
            var env = psi.Environment;
            foreach (string name in names)
            {
                string? key = env.Keys.FirstOrDefault(k => string.Compare(k, name, StringComparison.OrdinalIgnoreCase) == 0);
                if (key is not null)
                    env.Remove("MSBuildSDKsPath");
            }

            return psi;
        }
    }
}
