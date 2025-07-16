import asyncio

# --- Synchronous Helper Functions ---

def generate_initial_data(count):
    """Generates a list of dictionaries for initial processing."""
    print(f"SYNC: Generating {count} initial data items.")
    data_list = []
    for i in range(1, count + 1):
        data_list.append({"id": i, "value": f"item_{i}", "status": "pending"})
    return data_list

def format_report(processed_items, errors):
    """Formats a final report string from processed items and errors."""
    print(f"SYNC: Formatting report. Processed: {len(processed_items)}, Errors: {len(errors)}")
    
    report_lines = ["--- Processing Report ---"]
    report_lines.append(f"Successfully processed {len(processed_items)} items:")
    for item in processed_items:
        # Use .get() to avoid errors if a key is missing
        item_id = item.get("id", item.get("original_id", "N/A"))
        result = item.get("result", item.get("detail", "N/A"))
        report_lines.append(f"  - ID: {item_id}, Result: {result}")

    if errors:
        report_lines.append(f"Encountered {len(errors)} errors:")
        for err in errors:
            report_lines.append(f"  - Item ID: {err.get('id', 'N/A')}, Error: {err.get('error_message', 'Unknown error')}")
    else:
        report_lines.append("No errors encountered.")
    
    report_lines.append("--- End of Report ---")
    return "\n".join(report_lines)

# --- Asynchronous Helper Functions ---

async def simulate_io_delay(operation_name, duration_ms):
    """Simulates a non-blocking I/O operation like a network call or disk read."""
    print(f"ASYNC: Starting IO delay for '{operation_name}' ({duration_ms}ms)...")
    await asyncio.sleep(duration_ms / 1000.0)  # asyncio.sleep takes seconds
    print(f"ASYNC: ...Finished IO delay for '{operation_name}' ({duration_ms}ms).")
    return f"{operation_name} completed after {duration_ms}ms"

async def fetch_resource_data(resource_id, fail_on_id):
    """Simulates fetching a resource, with a possibility of a simulated failure."""
    print(f"ASYNC: Attempting to fetch resource '{resource_id}'...")
    await simulate_io_delay(f"fetch_resource_{resource_id}", 150)  # Simulate network latency
    
    if resource_id == fail_on_id:
        print(f"ASYNC: ...Fetch FAILED for resource '{resource_id}' (simulated error).")
        raise Exception(f"Simulated network error for resource {resource_id}")
    
    data_payload = {
        "id": resource_id,
        "content": f"Content for resource {resource_id}",
        "timestamp": "2023-10-27T10:00:00Z",
        "metadata": ["tag1", "tag2", "even_tag" if len(str(resource_id)) % 2 == 0 else "odd_tag"]
    }
    print(f"ASYNC: ...Successfully fetched resource '{resource_id}'.")
    return data_payload

async def process_data_item(item_data, processing_complexity):
    """Simulates a CPU-bound or I/O-bound task for processing a single data item."""
    print(f"ASYNC: Processing item ID '{item_data['id']}' with complexity {processing_complexity}...")
    delay_ms = 50 + (processing_complexity * 2)
    await simulate_io_delay(f"process_item_{item_data['id']}", delay_ms)

    result_dict = {"original_id": item_data["id"], "status": "processed"}
    
    # Simulate a processing failure for some items
    if item_data["id"] % 4 == 0:
        print(f"ASYNC: ...Processing FAILED for item ID '{item_data['id']}' (simulated internal error).")
        result_dict["status"] = "error"
        result_dict["error_detail"] = "Simulated processing failure."
    elif processing_complexity > 2:
        result_dict["detail"] = f"Complex processing successful. Value: {item_data['value'] * 2}"
        result_dict["value_length"] = len(item_data["value"])
    else:
        result_dict["detail"] = "Simple processing successful."
        result_dict["value_length"] = len(item_data["value"]) + 5

    print(f"ASYNC: ...Finished processing item ID '{item_data['id']}'. Status: {result_dict['status']}")
    return result_dict

# --- Main Asynchronous Orchestrator ---

async def main_async_workflow():
    """The main orchestrator that runs the entire asynchronous workflow."""
    print("--- ASYNC WORKFLOW STARTED ---")

    initial_items = generate_initial_data(5)  # Sync call
    print(f"Initial items: {initial_items}")

    setup_message = await simulate_io_delay("system_setup", 1000)  # Await simple async (1 second)
    print(setup_message)

    processed_results = []
    error_log = []
    iteration_count = 0

    print("--- Starting main processing loop ---")
    while iteration_count < len(initial_items):
        current_item = initial_items[iteration_count]
        print(f"LOOP Iteration {iteration_count + 1}: Processing item ID '{current_item['id']}'")

        try:
            # Every 3rd iteration, try fetching extra resources concurrently
            if iteration_count % 3 == 0:
                print(f"LOOP: Special handling for iteration {iteration_count + 1} - fetching extra resources.")
                # Create tasks to run concurrently
                resource_task1 = fetch_resource_data(f"extra_A_{current_item['id']}", "extra_A_3") # ID 3 will fail
                resource_task2 = fetch_resource_data(f"extra_B_{current_item['id']}", "non_existent_fail_id")
                
                print("LOOP: Gathering extra resources...")
                # return_exceptions=True prevents gather from stopping on the first error
                extra_resources = await asyncio.gather(resource_task1, resource_task2, return_exceptions=True)
                print(f"debug 2: {extra_resources}")
                print(f"LOOP: Extra resources gathered. Count: {len(extra_resources)}")
                
                # Process gathered resources
                for i, res_data in enumerate(extra_resources):
                    print(f"small debug: {res_data}")
                    if isinstance(res_data, Exception):
                        print(f"LOOP: Gathered resource {i + 1} had an error: {res_data}")
                        error_log.append({"id": f"extra_resource_{i}", "error_message": str(res_data)})
                    elif isinstance(res_data, dict):
                        print(f"LOOP: Gathered resource {i + 1} data: {res_data['content']}")
                        current_item[f"extra_info_{i}"] = res_data["metadata"]
                    else:
                        print(f"LOOP: Gathered resource {i + 1} has unexpected type: {type(res_data).__name__}")
                
                item_process_complexity = 3  # Higher complexity after fetching extra resources
                processing_result = await process_data_item(current_item, item_process_complexity)
                if processing_result["status"] == "processed":
                    processed_results.append(processing_result)
                else:
                    error_log.append({"id": current_item["id"], "error_message": processing_result["error_detail"]})

            # Even iterations (but not divisible by 3)
            elif iteration_count % 2 == 0:
                print(f"LOOP: Standard processing for item ID '{current_item['id']}'")
                fetched_single_resource = await fetch_resource_data(f"main_{current_item['id']}", "main_4") # ID 4 will fail
                print(f"LOOP: Main resource for item ID '{current_item['id']}': {fetched_single_resource['content']}")
                current_item['main_content_length'] = len(fetched_single_resource['content'])
                
                item_process_complexity = 1
                processing_result = await process_data_item(current_item, item_process_complexity)
                if processing_result["status"] == "processed":
                    processed_results.append(processing_result)
                else:
                    error_log.append({"id": current_item['id'], "error_message": processing_result["error_detail"]})
            
            # Odd iterations (but not divisible by 3)
            else:
                print(f"LOOP: Quick processing for item ID '{current_item['id']}'")
                await simulate_io_delay(f"quick_op_item_{current_item['id']}", 30)
                current_item['status'] = "quick_processed"
                current_item['quick_detail'] = f"Performed quick operation. Value: {current_item['value']}_quick"
                processed_results.append(current_item)
        
        except Exception as e:
            print(f"LOOP: EXCEPTION CAUGHT for item ID '{current_item['id']}': {e}")
            error_log.append({"id": current_item['id'], "error_message": str(e)})
        finally:
            print(f"LOOP: Finally block for item ID '{current_item['id']}' in iteration {iteration_count + 1}.")

        iteration_count += 1
        if iteration_count == 3:  # Test break
            print("LOOP: Reached iteration 3, breaking loop for demonstration.")
            break
    
    print("--- Main processing loop finished ---")
    print("--- Starting final batch of concurrent tasks ---")
    
    final_task_ids = ["final_alpha", "final_beta", "final_gamma_FAIL", "final_delta"]
    final_tasks_to_run = []
    for task_id_str in final_task_ids:
        # Create coroutine objects, don't await them yet for gather
        fail_id_for_fetch = "final_gamma_FAIL" if task_id_str == "final_gamma_FAIL" else "no_fail_expected"
        task_coro = fetch_resource_data(task_id_str, fail_id_for_fetch)
        final_tasks_to_run.append(task_coro)
    
    print(f"Final tasks created. Gathering all {len(final_tasks_to_run)} final tasks...")
    final_gathered_results = await asyncio.gather(*final_tasks_to_run, return_exceptions=True)
    print("--- Final batch tasks gathered ---")

    for i, res in enumerate(final_gathered_results):
        if isinstance(res, Exception):
            print(f"Final Task {i} Result (Error): {res}")
            error_log.append({"id": final_task_ids[i], "error_message": str(res)})
        elif isinstance(res, dict):
            print(f"Final Task {i} Result (Success for '{res['id']}'): {res['content'][:20]}...")
            processed_results.append({"id": res["id"], "result": "Final task success", "content_preview": res["content"][:10]})
        else:
            print(f"Final Task {i} Result (Unknown type): {res}")

    final_report = format_report(processed_results, error_log)  # Sync call
    print(final_report)

    print("--- ASYNC WORKFLOW FINISHED ---")
    return f"Workflow completed. Processed: {len(processed_results)}, Errors: {len(error_log)}"

# --- Run the main async workflow ---
if __name__ == "__main__":
    print("--- async_test.py starting ---")
    # asyncio.run() starts the event loop and runs the main coroutine
    final_status = asyncio.run(main_async_workflow())
    print(final_status)
    print("--- async_test.py finished ---")
