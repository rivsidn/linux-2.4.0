## 软中断

### 软中断注册

```c
//优先级从高到底顺序排列
open_softirq(HI_SOFTIRQ, tasklet_hi_action, NULL);
open_softirq(NET_TX_SOFTIRQ, net_tx_action, NULL);
open_softirq(NET_RX_SOFTIRQ, net_rx_action, NULL);
open_softirq(TASKLET_SOFTIRQ, tasklet_action, NULL);
```

### 软中断执行

```c
asmlinkage void do_softirq()
{
    //...
    h = softirq_vec;
    do {
        if (active & 1)
            h->action();
        h++;
        active >>= 1;
    } while (active);
    //...
}
```



## 后半段

`init_bh()`

`mark_bh()`-->`tasklet_hi_schedule()`-->

也就是说，后半段属于`HI_SOFTIRQ` 的一部分，所有的后半段都指向`bh_action()` 函数，通过获取锁确保能够串行处理。



